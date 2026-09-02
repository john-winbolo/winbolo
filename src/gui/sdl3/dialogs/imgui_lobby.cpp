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
 * Name:          imgui_lobby.cpp
 * Purpose:       ImGui Lobby dialog.
 *                Blocking modal loop that shows the lobby
 *                while waiting for the game to start.
 *********************************************************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cfloat>   /* FLT_MAX — unbounded max for window size constraints */
#include <cmath>    /* atan2 / floor — lobby start compass octant math */
#include <algorithm>  /* std::sort — used for chooser list order */
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <map>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImGui::CloseButton — proper X widget with hit area */
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "imgui_star_rating.h"
#include "imgui_nav_outline.h"
#include "imgui_controller_prompt.h"
#include "imgui_server_address.h"
#include "dialog_footer.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "../imgui_steam_nav.h"
#include "../glyphs.h"   /* glyphForActionAuto — controller footer legend */

#include "lobby/lobby_internal.h"
extern "C" {
#include "../../../steam/steam_input_actions.h"  /* SI_ACTION_MENU_* names */
#include "../sdl3draw.h"
#include "../../tiles.h"   /* map sprites for the recap scoreboard headers */
#include "../../gamefront.h"
#include "global.h"
#include "bolo_rand.h"
#include "client_sim.h"
#include "client_net.h"
#include "../../sound.h"
#include "server_sim.h"          /* serverSim* T1 wrappers for SP-host paths */
#include "lobby_bot_pools.h"     /* lobbyBotPool* — public utility */
#include "playername_validate.h" /* playerNameValidate — client-side bot name gate */
#include "../../../server/server_lifecycle.h"
#include "../../../server/server_dedicated_log.h"  /* last completed round's .wbv */
#include "../../../server/threads.h"  /* threadsWaitForMutex / Release — SP-host server calls */
#include "platform_net.h"
#include "../../../common/mp_diag_log.h"
#include "../flags.h"
#include "../sdl3imgui.h"
#include "../input_gamepad.h"  /* inputGamepadGetScrollDirection — right stick */
#include "../../ui_mode.h"
#include "../minimap_render.h"
#include "../../../bolo/public/client_mappreview.h"
#include "../../../bolo/public/wire_limits.h"
#include "../../../bolo/public/round_stats_derive.h"  /* roundStatsPickAwardSubset */
#include <errno.h>

/* stb_image entry points used by lobbyWbnGeneratePreview (defined in
 * src/third_party/stb/stb_image_impl.c). Declared at file scope so
 * they're visible to provider code earlier in the file. */
extern "C" {
    unsigned char *stbi_load_from_memory(const unsigned char *, int,
                                         int *, int *, int *, int);
    void stbi_image_free(void *);
}
#include "../map_preview_popup.h"
#include "../macos_pinch.h"  /* macOSPinchZoomConsume — trackpad pinch over the reel */
#include "../../lang.h"
#include "imgui_lobby.h"
#include "imgui_keyboard.h"
#include "imgui_messagebox.h"
#include "imgui_mapchooser.h"
#include "imgui_winbolonet.h"    /* sign-in section, for the recap's comment form */
#include "../../../winbolonet/http.h"
#include "../../../winbolonet/wbn_comments.h"  /* recap ratings + comments */
#include "../../../winbolonet/winbolonet_core.h"  /* winbolonetIsRunning() — gates WBN-only UI */
#include "cJSON.h"

}
#if !BOLO_MOBILE && BOLO_RECAP_CLIP_GIF
/* Single-header GIF encoder behind the recap's clip export; the one
 * implementation TU is src/third_party/msf_gif/msf_gif_impl.c. Carries its own
 * extern "C" guards, so it goes outside the block above. */
#include "../../../third_party/msf_gif/msf_gif.h"
#endif
#include "../wb_theme.h"
#include "../lobby_start_markers.h"  /* shared start-ownership marker helpers */

#define MAP_PREVIEW_SIZE 256

static const int DIALOG_W = 1024;
static const int DIALOG_H = 768;
/* Smallest lobby window a restored size is allowed to shrink to — below
 * this the two-column layout's own floors stop fitting. */
static const int DIALOG_MIN_W = 640;
static const int DIALOG_MIN_H = 480;

/* Players/map column split, as a signed offset off the automatic split, in
 * logical (UI-scale-independent) pixels — the layout multiplies it by the
 * scale it computed for the current window, so a scale change carries the
 * divider along instead of stranding it. Positive widens the left column.
 * One offset per right-panel view: the post-game replay wants the width and
 * the map view wants it back for the teams table, so a single remembered
 * position would have the player re-dragging the divider after every round
 * and again before the next one. Which view is up picks the offset the
 * layout applies and the one a drag moves.
 * Seeded from WINDOW/Lobby Split and WINDOW/Lobby Split Recap on first use
 * rather than at lobby entry: the in-game seam calls imguiLobbyRenderFrame
 * without going through imguiLobbyShow, and both paths have to come up on
 * the saved split. */
static float s_lobbySplitOffsetMap   = 0.0f;
static float s_lobbySplitOffsetRecap = 0.0f;
static bool  s_lobbySplitOffsetInit  = false;

/* ── Map chooser helpers ──────────────────────────────────────────
 * The map chooser is a separate draggable ImGui window opened from the
 * lobby's Map tab. Selection triggers PACKET_LOBBY_SET_MAP in MP, or a
 * direct serverSimReloadMap call when the lobby is SP-host. State
 * persists across re-opens so the user's last tab + position are
 * remembered for the session.
 *
 * State has to be declared before the helpers that reference it. */

static LobbyChooserState s_chooser = {};

/* Core reads the window's visibility and its focus edge-trigger; wbnmaps
 * flags a fired live-preview through previewPending. */
LobbyChooserState *lobbyChooser(void) {
    return &s_chooser;
}

void lobbyChooserReset(void) {
    s_chooser = LobbyChooserState{};
}

/* Map-browser state that outlives any one lobby session: the four
 * chooser instances, the tab the user was last on and the one-shot init
 * latch. Deliberately has no reset function — imguiLobbyFrameReset runs
 * on every lobby→game edge, so clearing this would tear down the preview
 * textures, re-scan the map directories once a round and throw away the
 * user's folder position, selection and search filter every time.
 *
 * Keeping the four instances separate means each tab remembers its own
 * folder / selection / search filter independently. */
typedef struct LobbyChooserTabs {
    /* Server Maps tab — routes its directory listing through
     * serverSimEnumerateMapDir so it reflects the server's actual map
     * library. */
    MapChooserState server        = {};
    /* Upload tab — always uses the LOCAL filesystem so the user can
     * browse their own files before sending them up. */
    MapChooserState upload        = {};
    /* Random tab — runs in randomTabOnly mode so the widget renders
     * generator controls + a live preview. Config changes ripple to the
     * server via genSeq edge detection against randomLastSeq. */
    MapChooserState random        = {};
    uint32_t        randomLastSeq = 0;
    /* Winbolo.net Maps tab — same widget the Upload / Server Maps tabs
     * use, with a listProvider that fetches folders from
     * /api/v1/maps/{id} instead of from a local directory. The WASM build
     * has no libcurl/WBN HTTP backend, so the whole tab — state,
     * providers, async fetch threads — is compiled out there. */
#ifndef __EMSCRIPTEN__
    MapChooserState wbn           = {};
#endif
    bool            inited        = false;
    int             activeTab     = 0; /* 0=server 1=upload 2=random 3=wbn */
} LobbyChooserTabs;

static LobbyChooserTabs s_chooserTabs = {};

/* wbnmaps drives the WinBolo.net tab's browser through this, which is why the
 * enclosing LobbyChooserTabs does not have to be published. */
MapChooserState *lobbyChooserWbnTab(void) {
    return &s_chooserTabs.wbn;
}

/* enumerate for the Server Maps provider. Routes through the server's
 * directory enumeration so the chooser browses the SERVER's map
 * library, not the local client's filesystem.
 *
 * Two modes:
 *   - In-process server (single-player host, or a host running the
 *     UI). Calls serverSimEnumerateMapDir directly — synchronous,
 *     no network round-trip.
 *   - Pure network client. Sends PACKET_LOBBY_MAP_LIST_REQ and
 *     populates state->maps from the cached response on the
 *     ClientSim. Until the response lands the list shows just the
 *     synthetic ".." (or the inbuilt Everard at root). */
static void lobbyServerMapsListProvider(MapChooserState *state,
                                         const char *relPath, void *ctx) {
    state->numMaps = 0;
    ServerSim *spSim = gameFrontGetServerSim();
    ClientSim *cs = (ClientSim *)ctx;
    bool inSub = (relPath && relPath[0] != '\0');

    /* Recursive-search branch: skip the synthetic "[..]" + Everard
     * pinning and just populate from a recursive walk. SP routes
     * directly through serverSimSearchMapDir; MP sends
     * PACKET_LOBBY_MAP_SEARCH_REQ and reads from the lobbyMapSearch*
     * cache. */
    if (state->searchRecursive && state->searchFilter[0] != '\0') {
        ServerMapEntry hits[MAP_CHOOSER_MAX_MAPS];
        int got = 0;
        if (spSim) {
            got = serverSimSearchMapDir(spSim,
                inSub ? relPath : NULL,
                state->searchFilter,
                hits, MAP_CHOOSER_MAX_MAPS);
            if (got < 0) got = 0;
        } else if (cs && clientSimHasTransport(cs)) {
            const char *want    = inSub ? relPath : "";
            const char *cachedP = clientSimGetLobbyMapSearchPath(cs);
            const char *cachedQ = clientSimGetLobbyMapSearchQuery(cs);
            const char *reqP    = clientSimGetLobbyMapSearchReqPath(cs);
            const char *reqQ    = clientSimGetLobbyMapSearchReqQuery(cs);
            bool ready    = clientSimGetLobbyMapSearchReady(cs);
            bool inFlight = clientSimGetLobbyMapSearchInFlight(cs);
            bool haveMatch = ready &&
                (SDL_strcmp(cachedP, want) == 0) &&
                (SDL_strcmp(cachedQ, state->searchFilter) == 0);
            bool sameReq = (SDL_strcmp(reqP, want) == 0) &&
                           (SDL_strcmp(reqQ, state->searchFilter) == 0);
            if (!haveMatch && !(inFlight && sameReq)) {
                clientSimNetSendLobbyMapSearchRequest(cs, want,
                                                      state->searchFilter);
            }
            if (haveMatch) {
                int cnt = clientSimGetLobbyMapSearchCount(cs);
                for (int i = 0; i < cnt && got < MAP_CHOOSER_MAX_MAPS; i++) {
                    SDL_strlcpy(hits[got].name,
                                clientSimGetLobbyMapSearchName(cs, i),
                                sizeof(hits[got].name));
                    hits[got].isFolder =
                        clientSimGetLobbyMapSearchIsFolder(cs, i);
                    hits[got].modTime =
                        clientSimGetLobbyMapSearchModTime(cs, i);
                    got++;
                }
            }
        }
        for (int i = 0; i < got && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
            MapChooserEntry *e = &state->maps[state->numMaps++];
            memset(e, 0, sizeof(*e));
            e->isFolder = hits[i].isFolder;
            e->modTime  = hits[i].modTime;
            /* Split the hit's relative path into folder + basename. The
             * row shows just the basename; the enclosing folder lives
             * in crumbsPath so the hover tooltip + preview breadcrumb
             * both surface it. */
            const char *hitPath = hits[i].name;
            const char *base    = SDL_strrchr(hitPath, '/');
            if (base) {
                size_t folderLen = (size_t)(base - hitPath);
                if (folderLen >= sizeof(e->crumbsPath)) {
                    folderLen = sizeof(e->crumbsPath) - 1;
                }
                /* Prepend relPath when we're searching a subfolder so
                 * the crumb reads relative to data/maps, not the
                 * already-narrowed search root. */
                if (inSub) {
                    SDL_snprintf(e->crumbsPath, sizeof(e->crumbsPath),
                                 "%s/%.*s", relPath,
                                 (int)folderLen, hitPath);
                } else {
                    SDL_strlcpy(e->crumbsPath, hitPath,
                                folderLen + 1);
                }
                base++;
            } else {
                base = hitPath;
                if (inSub) {
                    SDL_strlcpy(e->crumbsPath, relPath,
                                sizeof(e->crumbsPath));
                }
            }
            SDL_strlcpy(e->name, base, sizeof(e->name));
            size_t dlen = SDL_strlen(e->name);
            if (dlen > 4 &&
                SDL_strcasecmp(e->name + dlen - 4, ".map") == 0) {
                e->name[dlen - 4] = '\0';
            }
            /* On-disk path the lobby reload uses. Search returns
             * paths relative to <relPath>; if relPath is set we
             * have to re-prepend it. */
            if (inSub) {
                SDL_snprintf(e->path, sizeof(e->path),
                             "data/maps/%s/%s", relPath, hitPath);
            } else {
                SDL_snprintf(e->path, sizeof(e->path),
                             "data/maps/%s", hitPath);
            }
        }
        /* Server chunk arrival order isn't guaranteed alphabetical
         * once the response spans multiple packets; sort the file rows
         * before EmitFolderRows snapshots them so the final layout is
         * folders-first-alpha then files-alpha. */
        if (state->numMaps > 1) {
            std::sort(&state->maps[0],
                      &state->maps[state->numMaps],
                [](const MapChooserEntry &a, const MapChooserEntry &b) {
                    return SDL_strcasecmp(a.name, b.name) < 0;
                });
        }
        /* Aggregate the unique folders the file rows reference and
         * prepend them as folder rows. The Server Maps tab's
         * currentDir is data/maps-relative (no prefix), so we pass
         * "" — the folder row's `path` becomes the bare crumb and a
         * click sets currentDir to that. */
        mapChooserEmitFolderRowsForSearchHits(state, "");
        return;
    }

    /* Synthetic ".." or inbuilt Everard, same as before. */
    if (inSub) {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, "[..]", sizeof(e->name));
        SDL_strlcpy(e->path, relPath, sizeof(e->path));
        size_t plen = SDL_strlen(e->path);
        while (plen > 0 && e->path[plen - 1] != '/' &&
                           e->path[plen - 1] != '\\') {
            e->path[--plen] = '\0';
        }
        if (plen > 0) e->path[plen - 1] = '\0';
        e->isFolder = true;
        e->isParentUp = true;
    } else {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, langGetText(STR_MAPCHOOSER_EVERARD),
                    sizeof(e->name));
        e->path[0] = '\0';
    }

    ServerMapEntry serverEntries[MAP_CHOOSER_MAX_MAPS];
    int got = 0;
    if (spSim) {
        got = serverSimEnumerateMapDir(spSim,
            inSub ? relPath : NULL,
            serverEntries,
            MAP_CHOOSER_MAX_MAPS - state->numMaps);
        if (got < 0) got = 0;
    } else if (cs && clientSimHasTransport(cs)) {
        /* Network client. Check the cached response: if it matches
         * the requested path, copy entries. Otherwise (different
         * path OR no response yet) send a request and surface an
         * empty listing until the reply lands. The send is gated on
         * "not already in flight for this path" to avoid spamming
         * the server every frame while we wait. */
        const char *cachedPath = clientSimGetLobbyMapListPath(cs);
        const char *reqPath    = clientSimGetLobbyMapListReqPath(cs);
        bool ready  = clientSimGetLobbyMapListReady(cs);
        bool inFlight = clientSimGetLobbyMapListInFlight(cs);
        const char *want = inSub ? relPath : "";

        bool haveMatch = ready && (SDL_strcmp(cachedPath, want) == 0);
        bool sameReq   = (SDL_strcmp(reqPath, want) == 0);
        if (!haveMatch && !(inFlight && sameReq)) {
            clientSimNetSendLobbyMapListRequest(cs, want);
        }
        if (haveMatch) {
            int cnt = clientSimGetLobbyMapListCount(cs);
            for (int i = 0; i < cnt &&
                 got < MAP_CHOOSER_MAX_MAPS - state->numMaps; i++) {
                SDL_strlcpy(serverEntries[got].name,
                            clientSimGetLobbyMapListName(cs, i),
                            sizeof(serverEntries[got].name));
                serverEntries[got].isFolder =
                    clientSimGetLobbyMapListIsFolder(cs, i);
                serverEntries[got].modTime =
                    clientSimGetLobbyMapListModTime(cs, i);
                got++;
            }
        }
    }

    for (int i = 0; i < got && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        e->isFolder = serverEntries[i].isFolder;
        e->modTime  = serverEntries[i].modTime;
        if (e->isFolder) {
            SDL_strlcpy(e->name, serverEntries[i].name, sizeof(e->name));
            if (inSub) {
                SDL_snprintf(e->path, sizeof(e->path), "%s/%s",
                             relPath, serverEntries[i].name);
            } else {
                SDL_strlcpy(e->path, serverEntries[i].name, sizeof(e->path));
            }
        } else {
            /* Strip .map for the display name, keep full path. */
            SDL_strlcpy(e->name, serverEntries[i].name, sizeof(e->name));
            size_t nlen = SDL_strlen(e->name);
            if (nlen > 4 &&
                SDL_strcasecmp(e->name + nlen - 4, ".map") == 0) {
                e->name[nlen - 4] = '\0';
            }
            /* Path is relative to data/maps; prepend the on-disk
             * root so updatePreview's file load works. */
            if (inSub) {
                SDL_snprintf(e->path, sizeof(e->path),
                             "data/maps/%s/%s",
                             relPath, serverEntries[i].name);
            } else {
                SDL_snprintf(e->path, sizeof(e->path),
                             "data/maps/%s", serverEntries[i].name);
            }
            /* At root we already inserted Everard at index 0 —
             * skip the on-disk dup if present. */
            if (!inSub && SDL_strcasecmp(serverEntries[i].name,
                                          "Everard Island.map") == 0) {
                state->numMaps--;
            }
        }
    }

    /* Sort to match the chooser's normal order: ".." (if present)
     * pinned at index 0, then folders first, then files alphabetic
     * (case-insensitive). The synthetic Everard entry sorts in among
     * files by name. */
    int sortFrom = inSub ? 1 : 0;
    if (state->numMaps - sortFrom > 1) {
        std::sort(&state->maps[sortFrom],
                  &state->maps[state->numMaps],
            [](const MapChooserEntry &a, const MapChooserEntry &b) {
                if (a.isFolder != b.isFolder) return a.isFolder;
                return SDL_strcasecmp(a.name, b.name) < 0;
            });
    }
}

/* onSelect for the Server Maps provider. Click-to-preview: the moment
 * the user picks a row the map is pushed to the server. SP runs it
 * in-process; MP sends SET_MAP and the server stashes the previous
 * map so Cancel can revert. Set Map (PREVIEW_COMMIT) just frees the
 * stash. ctx is the ClientSim*. */
static void lobbyServerMapsOnSelect(MapChooserState *state, void *ctx) {
    ClientSim *cs = (ClientSim *)ctx;
    if (!cs) return;
    const char *sel = state->selectedPath;
    if (!sel) return;
    /* Empty path is the chooser's inbuilt-Everard marker. Stage the
     * real on-disk copy so the preview/commit cycle has something to
     * commit (otherwise "Use This Map" reverts). The MP branch below
     * strips the "data/maps/" prefix, yielding the server-relative
     * "Everard Island.map" it expects in SET_MAP. */
    if (!sel[0]) sel = "data/maps/Everard Island.map";
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[MAPPICK] server-maps onSelect sel='%s' sp=%d",
                sel, (int)clientSimIsSinglePlayer(cs));
    if (clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (sim) {
            threadsWaitForMutex();
            bool ok = serverSimReloadMap(sim, sel);
            threadsReleaseMutex();
            if (ok) s_chooser.previewPending = true;
        }
    } else {
        const char *relPath = sel;
        static const char kPrefix[] = "data/maps/";
        if (strncmp(relPath, kPrefix, sizeof(kPrefix) - 1) == 0) {
            relPath += sizeof(kPrefix) - 1;
        }
        clientSimNetSendLobbySetMap(cs, relPath);
        /* Also fetch the raw bytes so the chooser's own preview pane
         * can rasterise this map. SET_MAP updates the live lobby map
         * but does not feed the chooser preview; the streamed
         * MAP_PREVIEW response (drained by lobbyServerMapsPumpPreview)
         * does. */
        clientSimNetSendLobbyMapPreviewRequest(cs, relPath);
        s_chooser.previewPending = true;
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[MAPPICK] server-maps SET_MAP relPath='%s' previewPending=1",
                    relPath);
    }
}

/* onFolderJump for the Server Maps provider. The chooser prepends a
 * "Maps" root label to every breadcrumb (crumbsRootLabel), so strip
 * it back off before setting the dir. Bare "Maps" means "root" → "". */
static void lobbyServerMapsOnFolderJump(MapChooserState *state,
                                         const char *jumpPath, void *ctx) {
    (void)ctx;
    const char *jp = jumpPath;
    if (SDL_strncmp(jp, "Maps/", 5) == 0) {
        jp += 5;
    } else if (SDL_strcasecmp(jp, "Maps") == 0) {
        jp = "";
    }
    SDL_strlcpy(state->currentDir, jp, sizeof(state->currentDir));
}

/* generatePreview for the Server Maps provider. Runs on the
 * preview-cache worker thread. TODO: ask the server (or SP-host
 * ServerSim) for the compressed map bytes for `entryPath`, then
 * rasterise via minimapRenderPixels — the same code the Upload
 * provider uses. For SP-host this could just call
 * clientMapPreviewLoadFromFile on the resolved on-disk path; for
 * MP we'd need a new packet (e.g. PACKET_LOBBY_MAP_PREVIEW_REQ /
 * _DONE) so the server streams the compressed bytes back without
 * triggering a full SET_MAP. Returning false marks the row
 * unavailable; the cache won't retry. */
static bool lobbyServerMapsGeneratePreview(const char *entryPath,
                                            MapPreviewPixels *outBuf,
                                            void *ctx) {
    if (!entryPath || !*entryPath || !outBuf) return false;
    (void)ctx;

    /* The server is authoritative for what bytes a map file
     * contains, even when "server" == in-process serverSim. In
     * SP-host we ask the local serverSim for the bytes (it's the
     * one with the path-safety checks); in MP we'd send a
     * PACKET_LOBBY_MAP_PREVIEW_REQ packet. Either way the bytes
     * round-trip through the server API rather than the chooser
     * fs-walking the local data/maps/ directly — that way one
     * day a remote server with a different map library can swap
     * in transparently. The actual rasterisation happens on the
     * client (no server-side renderer dep). */

    /* entryPath comes in as "data/maps/<rel>" — the chooser keys
     * by that string for cache identity. Strip the prefix to get
     * the relPath the server API expects. */
    const char *relPath = entryPath;
    static const char kPrefix[] = "data/maps/";
    if (SDL_strncmp(entryPath, kPrefix, sizeof(kPrefix) - 1) == 0) {
        relPath += sizeof(kPrefix) - 1;
    }

    /* Any time we have a local ServerSim (SP, or host running the
     * UI), short-circuit through its API — matches the pattern
     * lobbyServerMapsListProvider uses. Pure network clients fall
     * through to the MP path below. */
    ServerSim *sim = gameFrontGetServerSim();
    if (sim) {
        uint8_t *mapBytes = NULL;
        size_t   mapLen   = 0;
        if (!serverSimReadMapFile(sim, relPath, &mapBytes, &mapLen)) {
            return false;
        }
        /* clientMapPreviewLoadFromBuffer expects the runtime
         * compressed format (basesCompressData + lzw-encoded map
         * tiles); we have the raw BMAPBOLO file bytes. mapRead is
         * the right parser, and it only knows how to read from a
         * FILE*, so spill the bytes to a worker-private temp file
         * and call clientMapPreviewLoadFromFile. The worker is
         * single-threaded, so one well-known temp name is safe. */
        const char *tmpPath = "data/preview_cache/.sm_tmp_load.map";
        SDL_CreateDirectory("data/preview_cache");
        FILE *fp = fopen(tmpPath, "wb");
        if (!fp) { free(mapBytes); return false; }
        size_t wrote = fwrite(mapBytes, 1, mapLen, fp);
        fclose(fp);
        free(mapBytes);
        if (wrote != mapLen) {
            SDL_RemovePath(tmpPath);
            return false;
        }
        MapPreview *mp = clientMapPreviewLoadFromFile(tmpPath);
        SDL_RemovePath(tmpPath);
        if (!mp) return false;
        size_t bufSz = MINIMAP_SIZE * MINIMAP_SIZE * 4;
        uint8_t *pixels = (uint8_t *)SDL_malloc(bufSz);
        if (!pixels) {
            clientMapPreviewDestroy(mp);
            return false;
        }
        minimapRenderPixels(mp, pixels, NULL, 0);
        clientMapPreviewDestroy(mp);
        outBuf->w      = MINIMAP_SIZE;
        outBuf->h      = MINIMAP_SIZE;
        outBuf->pixels = pixels;
        return true;
    }

    /* MP client. The per-row thumbnail cache (this worker-thread hook)
     * stays unimplemented for MP — it would need one streamed fetch per
     * visible row. Instead the selected map's preview is driven on the
     * main thread by lobbyServerMapsPumpPreview, which requests the
     * bytes via PACKET_LOBBY_MAP_PREVIEW_REQ on select and feeds the
     * streamed reply into mapChooserSetSelectedMapBytes (mirroring the
     * WBN tab). So return false here — no per-row thumbnail — but the
     * selected-map preview pane still fills in. */
    return false;
}

/* Main-thread pump for the Server Maps preview pane (MP only). Polls
 * the ClientSim's lobbyMapPreview* accumulator (filled async by the
 * CHANNEL_BULK preview receiver) and, once a full map's bytes have
 * arrived for the path we asked for, hands them to the chooser via
 * mapChooserSetSelectedMapBytes, which rasterises them in memory.
 * No-op for SP / in-process host — there generatePreview already serves
 * the preview synchronously off the local ServerSim. */
static void lobbyServerMapsPumpPreview(ClientSim *cs, SDL_Renderer *renderer) {
    if (!cs || gameFrontGetServerSim() != NULL) return;
    if (clientSimGetLobbyMapPreviewError(cs)) {
        clientSimClearLobbyMapPreview(cs);
        return;
    }
    if (!clientSimGetLobbyMapPreviewReady(cs)) return;

    const char *path = clientSimGetLobbyMapPreviewPath(cs);
    const char *want = clientSimGetLobbyMapPreviewReqPath(cs);
    const uint8_t *bytes = clientSimGetLobbyMapPreviewBytes(cs);
    uint32_t blen = clientSimGetLobbyMapPreviewLen(cs);
    /* Ignore a response that no longer matches the active request. */
    if (!path[0] || SDL_strcmp(path, want) != 0 || !bytes || blen == 0) {
        clientSimClearLobbyMapPreview(cs);
        return;
    }

    /* Display name = basename minus the .map suffix. */
    char disp[128];
    const char *base = SDL_strrchr(path, '/');
    base = base ? base + 1 : path;
    SDL_strlcpy(disp, base, sizeof(disp));
    size_t dl = SDL_strlen(disp);
    if (dl > 4 && SDL_strcasecmp(disp + dl - 4, ".map") == 0) {
        disp[dl - 4] = '\0';
    }

    /* Rasterise from memory. This used to spill the blob to
     * data/preview_cache/.sm_preview.map and have mapChooserSetSelectedFile
     * read it straight back off disk — a write and a re-read of bytes
     * already in hand, and the one step here that depends on a writable
     * filesystem, which the browser build does not really have (data/ is
     * MEMFS, and the cache directory has to be created at runtime). Both
     * failure branches were silent — no else on the fopen, none on a short
     * write — and the clear below runs either way, so a failed write
     * dropped the blob with nothing left to re-request it: the sole
     * PREVIEW_REQ goes out on the row click.
     *
     * mapChooserSetSelectedMapBytes takes the same raw .map image the WBN
     * tab feeds it and needs no file at all. It does stamp a synthetic
     * "wbnmem:" marker over selectedPath for that tab's loading-spinner
     * check, so preserve the row path the click already put there. */
    char keepPath[FILENAME_MAX];
    SDL_strlcpy(keepPath, s_chooserTabs.server.selectedPath, sizeof(keepPath));
    mapChooserSetSelectedMapBytes(&s_chooserTabs.server, renderer,
                                  bytes, (int)blen, disp);
    SDL_strlcpy(s_chooserTabs.server.selectedPath, keepPath,
                sizeof(s_chooserTabs.server.selectedPath));
    clientSimClearLobbyMapPreview(cs);
}

/* No tooltip on the Server Maps path label — the path always reads
 * "maps/..." which is already self-describing. */
static void lobbyServerMapsTooltipPrefix(MapChooserState *state, void *ctx) {
    (void)ctx;
    state->pathTooltipPrefix[0] = '\0';
}

/* onSelect for the Upload provider. SP loads the file directly (it's
 * already on the local fs the server reads from); MP streams it via
 * the upload protocol (gated on not-already-in-flight). ctx is cs. */
static void lobbyUploadOnSelect(MapChooserState *state, void *ctx) {
    ClientSim *cs = (ClientSim *)ctx;
    if (!cs) return;
    const char *picked = state->selectedPath;
    if (!picked) return;
    /* An empty path is the chooser's canonical "inbuilt Everard"
     * marker — the embedded map is hidden from the on-disk listing and
     * pinned as a pathless row. Selecting it must still stage a real
     * map through the preview/commit cycle; otherwise serverSimReloadMap
     * is never called, "Use This Map" commits whatever was staged
     * before, and the pick silently reverts. The inbuilt map is the
     * same bytes as the on-disk copy the preview falls back to, so
     * reload that. */
    if (!picked[0]) picked = "data/maps/Everard Island.map";
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[MAPPICK] upload onSelect picked='%s' sp=%d",
                picked, (int)clientSimIsSinglePlayer(cs));
    if (clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        bool ok = false;
        if (sim) {
            threadsWaitForMutex();
            ok = serverSimReloadMap(sim, picked);
            threadsReleaseMutex();
        }
        if (ok) {
            s_chooser.previewPending = true;
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[MAPPICK] upload SP reload ok previewPending=1");
        } else {
            WB_LOG_WARN(WB_LOG_CAT_GUI,
                        "[MAPPICK] upload SP reload FAILED for '%s'", picked);
        }
    } else if (clientSimHasTransport(cs)) {
        uint8_t upStatus = clientSimGetLobbyMapUploadStatus(cs);
        bool inFlight = (upStatus == 1 || upStatus == 2);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[MAPPICK] upload MP upStatus=%u inFlight=%d",
                    (unsigned)upStatus, (int)inFlight);
        if (!inFlight) {
            if (clientSimNetSendLobbyMapUpload(cs, picked)) {
                s_chooser.previewPending = true;
                WB_LOG_INFO(WB_LOG_CAT_GUI,
                            "[MAPPICK] upload kicked previewPending=1");
            } else {
                WB_LOG_WARN(WB_LOG_CAT_GUI,
                            "[MAPPICK] upload kick rejected for '%s'", picked);
            }
        }
    }
}

/* Tooltip on the Upload path label = absolute on-disk dir under cwd.
 * SDL_GetCurrentDirectory returns a malloc'd path with a trailing
 * separator; trim it and concat /data/maps so the hover label shows
 * exactly where local .map files should land. SDL returns NULL on
 * platforms without a meaningful cwd, in which case suppress the
 * tooltip. */
static void lobbyUploadTooltipPrefix(MapChooserState *state, void *ctx) {
    (void)ctx;
    char *cwd = SDL_GetCurrentDirectory();
    if (cwd) {
        size_t cwdLen = SDL_strlen(cwd);
        while (cwdLen > 0 &&
               (cwd[cwdLen - 1] == '/' || cwd[cwdLen - 1] == '\\')) {
            cwd[--cwdLen] = '\0';
        }
        SDL_snprintf(state->pathTooltipPrefix,
                     sizeof(state->pathTooltipPrefix),
                     "%s/data/maps", cwd);
        SDL_free(cwd);
    } else {
        state->pathTooltipPrefix[0] = '\0';
    }
}

/* generatePreview for the Upload provider. Runs on the chooser's
 * preview-cache worker thread. Loads the .map file from disk and
 * rasterises a 256x256 minimap via the shared renderer. The
 * resulting pixels are handed to the cache, which writes the PNG
 * to data/preview_cache/<hash>.png. */
static bool lobbyUploadGeneratePreview(const char *entryPath,
                                        MapPreviewPixels *outBuf,
                                        void *ctx) {
    (void)ctx;
    if (!entryPath || !*entryPath || !outBuf) return false;
    MapPreview *mp = clientMapPreviewLoadFromFile(entryPath);
    if (!mp) return false;
    size_t bufSz = MINIMAP_SIZE * MINIMAP_SIZE * 4;
    uint8_t *pixels = (uint8_t *)SDL_malloc(bufSz);
    if (!pixels) {
        clientMapPreviewDestroy(mp);
        return false;
    }
    minimapRenderPixels(mp, pixels, NULL, 0);
    clientMapPreviewDestroy(mp);
    outBuf->w      = MINIMAP_SIZE;
    outBuf->h      = MINIMAP_SIZE;
    outBuf->pixels = pixels;
    return true;
}

/* onFolderJump for the Upload provider. Upload's currentDir is the
 * absolute on-disk path (e.g. "data/maps/Sub"), and the breadcrumb
 * strips "data/maps/" before rendering and prepends "Maps". Reverse
 * both. Bare "Maps" lands at data/maps itself. */
static void lobbyUploadOnFolderJump(MapChooserState *state,
                                     const char *jumpPath, void *ctx) {
    (void)ctx;
    const char *jp = jumpPath;
    if (SDL_strncmp(jp, "Maps/", 5) == 0) {
        jp += 5;
    } else if (SDL_strcasecmp(jp, "Maps") == 0) {
        jp = "";
    }
    static const char kRoot[]     = "data/maps/";
    static const char kRootBare[] = "data/maps";
    if (jp[0] == '\0') {
        SDL_strlcpy(state->currentDir, kRootBare, sizeof(state->currentDir));
    } else if (strncmp(jp, kRoot, sizeof(kRoot) - 1) == 0 ||
               SDL_strcasecmp(jp, kRootBare) == 0) {
        SDL_strlcpy(state->currentDir, jp, sizeof(state->currentDir));
    } else {
        SDL_snprintf(state->currentDir, sizeof(state->currentDir),
                     "data/maps/%s", jp);
    }
}

/* ── Winbolo.net Maps tab — state and async folder fetch ─────────
 * The entire WBN tab depends on the libcurl-backed WBN HTTP API
 * (httpGetBaseUrl / wbn_api_get / wbn_api_download_to_memory*), which
 * the WASM build does not link. The tab is already runtime-gated on
 * winbolonetIsRunning() (false in WASM), so it is dead-but-linked
 * there; compile it out entirely to keep wasm-ld's undefined-symbol
 * set clean.
 *
 * The tab browses the WBN REST catalogue. Folder listings and
 * search results are fetched on a detached std::thread; the UI
 * thread renders from a mutex-guarded parsed snapshot. Picking a
 * map sends PACKET_LOBBY_PREVIEW_WBN to the host, which downloads
 * server-side (see transport_udp_server.c's wbnDownloadWorker).
 * The preview itself rolls in over the normal MAP_CHANGE flow —
 * no client-side download or local file write here. */
#ifndef __EMSCRIPTEN__

struct WbnMapsCrumb { int id; std::string name; };
/* Forward declare so WbnMapsEntry can carry the parent folder id. */
struct WbnMapsFolder { int id; std::string name; int count; };
struct WbnMapsEntry  {
    int id;
    std::string name;
    float rating;     /* < 0 = none */
    int numRatings;
    std::string owner;
    std::string folderName;   /* search results only: full slash-
                               * separated path joined from the
                               * response's path[] array (root
                               * "Collections" crumb stripped) */
    int         folderId = 0; /* search results only: leaf folder id */
    int64_t     uploadedNs = 0; /* response's "uploaded" timestamp
                                 * parsed to ns since UNIX epoch, or
                                 * 0 if missing/unparseable. Used as
                                 * the row's modTime so the "Created"
                                 * column lights up. */
};

/* The WinBolo.net catalogue the tab browses: the cached folder listing,
 * the path → id map built up alongside it, and a separate search-result
 * cache. Deliberately has no reset function — imguiLobbyFrameReset runs
 * on every lobby→game edge, and the chooser keeps this tab's folder
 * position and selection across sessions (see LobbyChooserTabs), so
 * clearing the listing behind it would leave the tab sitting on an empty
 * folder and re-fetch from WinBolo.net once a round.
 *
 * Written by detached fetch threads under the mutex for the life of the
 * process, and never assigned as a whole: the mutex and the two pairs of
 * atomics are not assignable, and a detached thread cannot be joined to
 * make a whole-struct write safe. */
typedef struct LobbyWbnMapsCache {
    std::mutex                 mutex;
    std::atomic<bool>          fetching{false};
    std::atomic<uint32_t>      fetchSeq{0}; /* invalidates late results */
    int                        currentFolderId = 0; /* 0 = root collection */
    std::string                currentPath;         /* canonical friendly path of cached folder */
    std::vector<WbnMapsCrumb>  crumbs;
    std::vector<WbnMapsFolder> subfolders;
    std::vector<WbnMapsEntry>  entries;
    std::string                error;
    bool                       hasData = false;
    /* Path → folder ID. Built up as folders are fetched: each response's
     * subfolders[] gives us (childName, childId) pairs which combine
     * with the parent path to form each child's full friendly path.
     * "" maps to 0 (the root collection). */
    std::map<std::string,int>  pathToId;
    /* Separate search-result cache so clearing the recursive-search
     * checkbox doesn't blow away the folder listing the user was just
     * browsing. Populated by wbnMapsParseSearchJson when the search
     * branch fires. */
    std::string                searchQuery; /* last query we got results for */
    std::vector<WbnMapsEntry>  searchResults;
    bool                       searchHasData = false;
    std::atomic<bool>          searchFetching{false};
    std::atomic<uint32_t>      searchFetchSeq{0};
    std::string                searchError;
} LobbyWbnMapsCache;

static LobbyWbnMapsCache s_wbnMaps;

/* Extract an int from a cJSON node that may be either a JSON
 * number or a JSON string. The WBN root listing returns id and
 * count as quoted strings ("355"); deeper folder listings return
 * them as bare numbers (3837). Accept both so the same parse
 * works at every level. Returns 0 when the node is missing or
 * not parseable. */
static int wbnJsonInt(const cJSON *node) {
    if (!node) return 0;
    if (cJSON_IsNumber(node)) return node->valueint;
    if (cJSON_IsString(node) && node->valuestring) {
        return SDL_atoi(node->valuestring);
    }
    return 0;
}

/* Parse a "YYYY-MM-DD HH:MM:SS" timestamp (UTC, the format the WBN
 * REST API uses for upload times) into SDL_Time = ns since the UNIX
 * epoch. Returns 0 on missing/malformed input so callers can flag
 * the row's modTime as unknown. */
static int64_t wbnParseUploadedToNs(const char *s) {
    if (!s) return 0;
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (SDL_sscanf(s, "%d-%d-%d %d:%d:%d",
                   &Y, &M, &D, &h, &m, &sec) < 5) {
        return 0;
    }
    if (Y < 1970 || M < 1 || M > 12 || D < 1 || D > 31) return 0;
    /* Compute days since UNIX epoch (1970-01-01) via the proleptic
     * Gregorian calendar — algorithm from "Date Algorithms" by
     * Howard Hinnant, treats March as the first month so leap-day
     * lands at the end of the year. Avoids mktime which uses the
     * local timezone (we want UTC to match the API's wall-clock
     * timestamps). */
    int y = Y - (M <= 2 ? 1 : 0);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned mp  = (unsigned)(M + (M > 2 ? -3 : 9));
    unsigned doy = (153 * mp + 2) / 5 + (unsigned)D - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t secs = days * 86400 + (int64_t)h * 3600
                 + (int64_t)m * 60 + sec;
    return secs * 1000000000LL;
}

static void wbnMapsParseFolderJson(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
        s_wbnMaps.error = langGetText(STR_DLGLOBBY_WBN_ERR_BADRESPONSE);
        s_wbnMaps.hasData = false;
        return;
    }
    std::vector<WbnMapsCrumb>  crumbs;
    std::vector<WbnMapsFolder> subs;
    std::vector<WbnMapsEntry>  entries;
    cJSON *folderId = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *path     = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (cJSON_IsArray(path)) {
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, path) {
            WbnMapsCrumb c;
            cJSON *cid = cJSON_GetObjectItemCaseSensitive(it, "id");
            cJSON *cnm = cJSON_GetObjectItemCaseSensitive(it, "name");
            c.id = wbnJsonInt(cid);
            c.name = cJSON_IsString(cnm) && cnm->valuestring
                       ? cnm->valuestring : "";
            crumbs.push_back(std::move(c));
        }
    }
    cJSON *subfolders = cJSON_GetObjectItemCaseSensitive(root, "subfolders");
    if (cJSON_IsArray(subfolders)) {
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, subfolders) {
            WbnMapsFolder f;
            cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *ct = cJSON_GetObjectItemCaseSensitive(it, "count");
            f.id    = wbnJsonInt(id);
            f.name  = cJSON_IsString(nm) && nm->valuestring
                        ? nm->valuestring : "";
            f.count = wbnJsonInt(ct);
            subs.push_back(std::move(f));
        }
    }
    cJSON *maps = cJSON_GetObjectItemCaseSensitive(root, "maps");
    if (cJSON_IsArray(maps)) {
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, maps) {
            WbnMapsEntry e;
            cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *rt = cJSON_GetObjectItemCaseSensitive(it, "rating");
            cJSON *nr = cJSON_GetObjectItemCaseSensitive(it, "num_ratings");
            cJSON *up = cJSON_GetObjectItemCaseSensitive(it, "uploaded");
            e.id         = wbnJsonInt(id);
            e.name       = cJSON_IsString(nm) && nm->valuestring
                             ? nm->valuestring : "";
            e.rating     = cJSON_IsNumber(rt) ? (float)rt->valuedouble : -1.0f;
            e.numRatings = wbnJsonInt(nr);
            e.uploadedNs = (cJSON_IsString(up) && up->valuestring)
                ? wbnParseUploadedToNs(up->valuestring) : 0;
            entries.push_back(std::move(e));
        }
    }
    int newFolderId = wbnJsonInt(folderId);
    cJSON_Delete(root);

    /* Build canonical friendly path from crumbs (skip the root
     * collection crumb — its id is null in the JSON). Joined with
     * "/" so it matches what the chooser stores in currentDir for
     * filesystem-shaped tabs.  e.g. "Collections" → ""
     *      "Collections / ClassicMap's Maps" → "ClassicMap's Maps"
     *      "Collections / ClassicMap's Maps / classics-popular"
     *        → "ClassicMap's Maps/classics-popular"  */
    std::string canonicalPath;
    for (const auto &c : crumbs) {
        if (c.id == 0 && c.name == "Collections") continue;
        if (!canonicalPath.empty()) canonicalPath += '/';
        canonicalPath += c.name;
    }

    {
        std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
        s_wbnMaps.crumbs        = std::move(crumbs);
        s_wbnMaps.subfolders    = subs;          /* copy — also used below */
        s_wbnMaps.entries       = std::move(entries);
        s_wbnMaps.currentFolderId = newFolderId;
        s_wbnMaps.currentPath   = canonicalPath;
        s_wbnMaps.error.clear();
        s_wbnMaps.hasData       = true;

        /* Stamp every (path, id) pair we now know — current folder
         * and each subfolder — so the listProvider can resolve
         * any seen path back to a numeric ID for fetching. */
        s_wbnMaps.pathToId[canonicalPath] = newFolderId;
        for (const auto &f : subs) {
            std::string childPath = canonicalPath.empty()
                ? f.name : (canonicalPath + "/" + f.name);
            s_wbnMaps.pathToId[childPath] = f.id;
        }
    }
    /* Log the first few subfolders so we can confirm what the
     * API is actually returning for id/count without dumping a
     * 250-entry blob. Drops the mutex first since this can be
     * chatty. */
    WB_LOG_INFO(WB_LOG_CAT_GUI,
        "[WBN-TAB] parsed folder id=%d path='%s' subs=%zu entries=%zu",
        newFolderId, canonicalPath.c_str(),
        subs.size(), entries.size());
    for (size_t i = 0; i < subs.size() && i < 6; i++) {
        WB_LOG_INFO(WB_LOG_CAT_GUI,
            "[WBN-TAB]   sub[%zu] id=%d count=%d name='%s'",
            i, subs[i].id, subs[i].count, subs[i].name.c_str());
    }
}

static void wbnMapsParseSearchJson(const char *json, const char *query) {
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
        s_wbnMaps.searchError = langGetText(STR_DLGLOBBY_WBN_ERR_BADSEARCHRESPONSE);
        return;
    }
    std::vector<WbnMapsEntry> entries;
    cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    if (cJSON_IsArray(results)) {
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, results) {
            WbnMapsEntry e;
            cJSON *id = cJSON_GetObjectItemCaseSensitive(it, "id");
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
            cJSON *rt = cJSON_GetObjectItemCaseSensitive(it, "rating");
            cJSON *nr = cJSON_GetObjectItemCaseSensitive(it, "num_ratings");
            cJSON *ow = cJSON_GetObjectItemCaseSensitive(it, "owner");
            cJSON *pa = cJSON_GetObjectItemCaseSensitive(it, "path");
            cJSON *up = cJSON_GetObjectItemCaseSensitive(it, "uploaded");
            e.id         = wbnJsonInt(id);
            e.name       = cJSON_IsString(nm) && nm->valuestring
                             ? nm->valuestring : "";
            e.rating     = cJSON_IsNumber(rt) ? (float)rt->valuedouble : -1.0f;
            e.numRatings = wbnJsonInt(nr);
            e.owner      = cJSON_IsString(ow) && ow->valuestring
                             ? ow->valuestring : "";
            e.uploadedNs = (cJSON_IsString(up) && up->valuestring)
                ? wbnParseUploadedToNs(up->valuestring) : 0;
            /* /maps/search returns a "path" array of breadcrumb
             * segments (root → leaf), each {id, name}. Join the
             * segment names with '/' for folderName (skipping the
             * synthetic "Collections" root crumb whose id is null);
             * folderId = the leaf folder's id (the actual enclosing
             * folder). Each prefix gets stamped into s_wbnMaps.pathToId
             * just below so breadcrumb clicks navigate without a
             * second round-trip. */
            if (cJSON_IsArray(pa)) {
                cJSON *seg = nullptr;
                cJSON_ArrayForEach(seg, pa) {
                    cJSON *sid = cJSON_GetObjectItemCaseSensitive(seg, "id");
                    cJSON *snm = cJSON_GetObjectItemCaseSensitive(seg, "name");
                    int    segId = wbnJsonInt(sid);
                    const char *segName =
                        cJSON_IsString(snm) && snm->valuestring
                            ? snm->valuestring : "";
                    if (segId == 0 &&
                        SDL_strcasecmp(segName, "Collections") == 0)
                        continue;
                    if (!e.folderName.empty()) e.folderName += '/';
                    e.folderName += segName;
                    e.folderId = segId;
                }
            }
            entries.push_back(std::move(e));
        }
    }
    /* Walk the path[] arrays a second time to stamp every prefix
     * (not just the leaf) into s_wbnMaps.pathToId — breadcrumb buttons
     * under the preview let the user click any segment, so each
     * intermediate "Collections/X" → folderId must resolve too. */
    std::vector<std::pair<std::string,int>> prefixIds;
    if (cJSON_IsArray(results)) {
        cJSON *it = nullptr;
        cJSON_ArrayForEach(it, results) {
            cJSON *pa = cJSON_GetObjectItemCaseSensitive(it, "path");
            if (!cJSON_IsArray(pa)) continue;
            std::string acc;
            cJSON *seg = nullptr;
            cJSON_ArrayForEach(seg, pa) {
                cJSON *sid = cJSON_GetObjectItemCaseSensitive(seg, "id");
                cJSON *snm = cJSON_GetObjectItemCaseSensitive(seg, "name");
                int segId = wbnJsonInt(sid);
                const char *segName =
                    cJSON_IsString(snm) && snm->valuestring
                        ? snm->valuestring : "";
                if (segId == 0 &&
                    SDL_strcasecmp(segName, "Collections") == 0)
                    continue;
                if (!acc.empty()) acc += '/';
                acc += segName;
                if (segId > 0) prefixIds.emplace_back(acc, segId);
            }
        }
    }
    cJSON_Delete(root);
    {
        std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
        for (const auto &p : prefixIds) {
            s_wbnMaps.pathToId[p.first] = p.second;
        }
        s_wbnMaps.searchResults  = std::move(entries);
        s_wbnMaps.searchQuery    = query ? query : "";
        s_wbnMaps.searchHasData  = true;
        s_wbnMaps.searchError.clear();
    }
}

/* ── SP-host WBN map fetch ───────────────────────────────────────
 * Single-player host has no UDP server to delegate the download
 * to, so this client process does the fetch itself on a worker
 * thread (mirrors the server's PREVIEW_WBN handler) and then
 * applies the bytes to the local SP ServerSim on the main thread
 * via spWbnPoll. Cancel-on-supersede uses the http.c cancellable
 * API so rapid map clicks abort the in-flight curl call. */
struct SpWbnResult {
    uint32_t mapId;
    int      httpStatus; /* 200 ok; -2 cancelled */
    std::string mapName;
    std::vector<uint8_t> bytes;
    std::string err;
    bool     valid;
};

typedef struct LobbySpWbnState {
    std::mutex             mutex;
    std::atomic<bool>      fetching{false};
    std::atomic<uint32_t>  fetchSeq{0};
    volatile int           cancel = 0; /* CURLOPT_XFERINFO sink */
    SpWbnResult            result;
} LobbySpWbnState;

/* Cleared a field at a time by lobbySpWbnReset and never as a whole: the
 * mutex and the two atomics are not assignable, and the worker is detached
 * so there is no join that would make a whole-struct write safe. */
static LobbySpWbnState s_spWbn;

/* Drop a download the lobby is walking away from, so a map's bytes do not
 * sit in the result slot until the next single-player pick drains it. The
 * worker is detached and cannot be stopped, so the fetch is superseded the
 * same way spWbnSubmit supersedes one: the cancel flag aborts curl mid
 * transfer and the seq bump makes any completion that still lands get
 * dropped. The flag stays raised — every attempt lowers it for itself
 * before the transfer starts. fetching belongs to the worker, which clears
 * it on its way out whichever branch it takes. */
void lobbySpWbnReset(void) {
    s_spWbn.cancel = 1;
    ++s_spWbn.fetchSeq;
    {
        std::lock_guard<std::mutex> lk(s_spWbn.mutex);
        s_spWbn.result = SpWbnResult{};
    }
}

static void spWbnSubmit(uint32_t mapId) {
    /* Supersede any in-flight call. The cancel flag aborts curl;
     * the seq bump invalidates the completion. */
    s_spWbn.cancel = 1;
    uint32_t seq = ++s_spWbn.fetchSeq;
    /* Drop any undrained previous result. */
    {
        std::lock_guard<std::mutex> lk(s_spWbn.mutex);
        s_spWbn.result = SpWbnResult{};
    }
    std::thread([mapId, seq]() {
        /* Wait briefly for previous thread to clear the fetching
         * flag — both threads race the same flag. */
        for (int i = 0; i < 50 && s_spWbn.fetching.load(); i++) {
            SDL_Delay(10);
        }
        s_spWbn.fetching.store(true);
        s_spWbn.cancel = 0; /* reset for this attempt */

        SpWbnResult out;
        out.mapId = mapId;

        char infoPath[64];
        SDL_snprintf(infoPath, sizeof(infoPath), "maps/info/%u",
                     (unsigned)mapId);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] info fetch: /api/v1/%s", infoPath);
        char *infoJson = nullptr;
        int infoStatus = wbn_api_get(infoPath, &infoJson);
        if (seq != s_spWbn.fetchSeq.load()) {
            free(infoJson);
            s_spWbn.fetching.store(false);
            return;
        }
        if (infoStatus == 200 && infoJson) {
            cJSON *root = cJSON_Parse(infoJson);
            if (root) {
                cJSON *nm = cJSON_GetObjectItemCaseSensitive(root, "name");
                if (cJSON_IsString(nm) && nm->valuestring) {
                    out.mapName = nm->valuestring;
                }
                cJSON_Delete(root);
            }
        }
        free(infoJson);
        if (out.mapName.empty()) {
            char fallback[32];
            SDL_snprintf(fallback, sizeof(fallback), "wbn_%u",
                         (unsigned)mapId);
            out.mapName = fallback;
        }

        char filePath[64];
        SDL_snprintf(filePath, sizeof(filePath), "maps/file/%u",
                     (unsigned)mapId);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] file fetch: /api/v1/%s", filePath);
        uint8_t *bytes = nullptr;
        size_t   bytesLen = 0;
        int dlStatus = wbn_api_download_to_memory_cancellable(
            filePath, &bytes, &bytesLen, &s_spWbn.cancel);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] file fetch result: /api/v1/%s -> %d (%zu bytes)",
                    filePath, dlStatus, bytesLen);

        if (seq != s_spWbn.fetchSeq.load() || dlStatus == -2) {
            free(bytes);
            s_spWbn.fetching.store(false);
            return;
        }
        out.httpStatus = dlStatus;
        if (dlStatus == 200 && bytes) {
            out.bytes.assign(bytes, bytes + bytesLen);
        } else if (dlStatus == -1) {
            out.err = langGetText(STR_DLGLOBBY_WBN_ERR_NETERROR);
        } else {
            char msg[64];
            MessageArgs args = {};
            args.number = dlStatus;
            SDL_snprintf(msg, sizeof(msg), "%s",
                         langGetTextFmt(STR_DLGLOBBY_WBN_ERR_HTTPERROR, &args));
            out.err = msg;
        }
        free(bytes);
        out.valid = true;
        {
            std::lock_guard<std::mutex> lk(s_spWbn.mutex);
            s_spWbn.result = std::move(out);
        }
        s_spWbn.fetching.store(false);
    }).detach();
}

/* Main-thread completion handler for SP picks. Drains the result
 * and applies it to the local sim. Called once per frame while
 * the WBN tab is visible. */
static void spWbnPoll(ClientSim *cs, SDL_Renderer *renderer) {
    if (!cs) return;
    SpWbnResult res;
    {
        std::lock_guard<std::mutex> lk(s_spWbn.mutex);
        if (!s_spWbn.result.valid) return;
        res = std::move(s_spWbn.result);
        s_spWbn.result = SpWbnResult{};
    }
    if (res.httpStatus != 200 || res.bytes.empty()) {
        clientSimSetLobbyWbnPreviewStatus(cs, 3);
        clientSimSetLobbyWbnPreviewErrMsg(cs,
            res.err.empty() ? langGetText(STR_DLGLOBBY_WBN_ERR_MAPFAILED) : res.err.c_str());
        WB_LOG_WARN(WB_LOG_CAT_GUI,
                    "[WBN-SP] fetch failed: %s",
                    res.err.empty() ? "(unknown)" : res.err.c_str());
        return;
    }

    /* Sanitise & ensure .map extension. Used for both the SP
     * commit target and the upload name announced to the server
     * in MP. */
    auto sanitise = [](std::string &s){
        for (auto &c : s) {
            unsigned char u = (unsigned char)c;
            if (u < 0x20 || c == '/' || c == '\\' || c == ':' ||
                c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|') c = '_';
        }
        if (s.empty()) s = "wbnmap";
    };
    sanitise(res.mapName);
    std::string safeName = res.mapName;
    if (safeName.size() < 4 ||
        SDL_strcasecmp(safeName.c_str() + safeName.size() - 4, ".map") != 0) {
        safeName += ".map";
    }

    /* Drive the WBN chooser's right-side preview straight from the
     * downloaded bytes — no temp file. The chooser's row carries a
     * synthetic "wbn:<id>" path which isn't a real file, so the
     * normal preview-on-click attempt fails; this hands it the map
     * image in RAM instead, which it converts and renders in place. */
    {
        /* displayName drops the trailing .map for the panel title —
         * matches the other tabs. */
        std::string displayName = res.mapName;
        if (displayName.size() >= 4 &&
            SDL_strcasecmp(displayName.c_str() + displayName.size() - 4,
                            ".map") == 0) {
            displayName.resize(displayName.size() - 4);
        }
        mapChooserSetSelectedMapBytes(
            &s_chooserTabs.wbn, renderer,
            reinterpret_cast<const uint8_t *>(res.bytes.data()),
            (int)res.bytes.size(), displayName.c_str());
    }

    /* MP host: chunked upload state machine on the transport.
     * SP host: the wrapper's local-transport branch installs the
     * bytes synchronously onto spServerSim (no chunked transfer). */
    if (!clientSimIsSinglePlayer(cs)) {
        uint8_t upStatus = clientSimGetLobbyMapUploadStatus(cs);
        if (upStatus == 1 || upStatus == 2) {
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[WBN-MP] another upload is in flight; ignoring pick");
            return;
        }
    }
    if (res.bytes.size() > LOBBY_MAP_UPLOAD_MAX_BYTES) {
        clientSimSetLobbyWbnPreviewStatus(cs, 3);
        clientSimSetLobbyWbnPreviewErrMsg(cs,
            langGetText(STR_DLGLOBBY_WBN_ERR_TOOBIG));
        return;
    }

    char displayName[MAP_STR_SIZE];
    SDL_strlcpy(displayName, safeName.c_str(), sizeof(displayName));
    {
        size_t dlen = SDL_strlen(displayName);
        if (dlen >= 4 &&
            SDL_strcasecmp(displayName + dlen - 4, ".map") == 0) {
            displayName[dlen - 4] = '\0';
        }
    }

    const char *wireName =
        clientSimIsSinglePlayer(cs) ? displayName : safeName.c_str();
    if (!clientSimNetSendLobbyMapUploadBytes(
            cs,
            reinterpret_cast<const uint8_t *>(res.bytes.data()),
            res.bytes.size(), wireName)) {
        if (clientSimIsSinglePlayer(cs)) {
            WB_LOG_WARN(WB_LOG_CAT_GUI,
                        "[WBN-SP] map install rejected bytes");
        }
        clientSimSetLobbyWbnPreviewStatus(cs, 3);
        if (!clientSimIsSinglePlayer(cs)) {
            clientSimSetLobbyWbnPreviewErrMsg(cs,
                langGetText(STR_DLGLOBBY_WBN_ERR_OOM));
        }
        return;
    }
    clientSimSetLobbyWbnPreviewStatus(cs, 2);
    s_chooser.previewPending = true;
    if (clientSimIsSinglePlayer(cs)) {
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] applied '%s' (%zu bytes)",
                    displayName, res.bytes.size());
    } else {
        WB_LOG_INFO(WB_LOG_CAT_GUI,
            "[WBN-MP] uploading '%s' (%u bytes)",
            safeName.c_str(), (unsigned)res.bytes.size());
    }
}

static void wbnMapsKickFolderFetch(int folderId) {
    if (s_wbnMaps.fetching.exchange(true)) return; /* one at a time */
    uint32_t seq = ++s_wbnMaps.fetchSeq;
    std::thread([folderId, seq]() {
        char path[64];
        if (folderId > 0) SDL_snprintf(path, sizeof(path), "maps/%d", folderId);
        else              SDL_strlcpy(path, "maps", sizeof(path));
        const char *base = httpGetBaseUrl();
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-TAB] folder fetch: %s/api/v1/%s",
                    (base && *base) ? base : "(no base)", path);
        char *resp = nullptr;
        int status = wbn_api_get(path, &resp);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-TAB] folder fetch result: %s/api/v1/%s -> %d",
                    (base && *base) ? base : "(no base)", path, status);
        if (seq == s_wbnMaps.fetchSeq.load()) {
            if (status == 200 && resp) {
                wbnMapsParseFolderJson(resp);
            } else {
                std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
                char err[512];
                SDL_snprintf(err, sizeof(err),
                             "Folder fetch failed (HTTP %d) for %s/api/v1/%s",
                             status,
                             (base && *base) ? base : "(no base)",
                             path);
                s_wbnMaps.error = err;
            }
        }
        free(resp);
        s_wbnMaps.fetching.store(false);
    }).detach();
}

static void wbnMapsKickSearchFetch(const char *queryRaw) {
    if (!queryRaw || !*queryRaw) return;
    if (s_wbnMaps.searchFetching.exchange(true)) return;
    uint32_t seq = ++s_wbnMaps.searchFetchSeq;
    std::string qcopy = queryRaw;
    /* Very basic URL-encode of space and a few common specials so
     * typical map titles work without pulling in a full encoder. */
    std::string enc;
    enc.reserve(qcopy.size() * 3);
    for (unsigned char c : qcopy) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            enc.push_back((char)c);
        } else {
            char hex[4];
            SDL_snprintf(hex, sizeof(hex), "%%%02X", c);
            enc.append(hex);
        }
    }
    std::string path = "maps/search?name=" + enc;
    std::thread([path, qcopy, seq]() {
        const char *base = httpGetBaseUrl();
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-TAB] search fetch: %s/api/v1/%s",
                    (base && *base) ? base : "(no base)", path.c_str());
        char *resp = nullptr;
        int status = wbn_api_get(path.c_str(), &resp);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-TAB] search fetch result: %s/api/v1/%s -> %d",
                    (base && *base) ? base : "(no base)", path.c_str(),
                    status);
        if (seq == s_wbnMaps.searchFetchSeq.load()) {
            if (status == 200 && resp) {
                /* Dump the first ~600 chars of the raw response
                 * so we can see exactly what shape the search
                 * endpoint returns (folder fields might differ
                 * from the /maps/<id> shape we modelled on). */
                WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-TAB] search raw response (first 600c): %.600s",
                    resp);
                wbnMapsParseSearchJson(resp, qcopy.c_str());
            } else {
                std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
                char err[512];
                SDL_snprintf(err, sizeof(err),
                             "Search failed (HTTP %d) for %s/api/v1/%s",
                             status,
                             (base && *base) ? base : "(no base)",
                             path.c_str());
                s_wbnMaps.searchError = err;
            }
        }
        free(resp);
        s_wbnMaps.searchFetching.store(false);
    }).detach();
}

/* listProvider for the Winbolo.net Maps tab. The chooser hands us
 * relPath (the chooser's currentDir — a friendly slash-separated
 * path like "ClassicMap's Maps/classics-popular"). We:
 *   - resolve it to a numeric folder ID via s_wbnMaps.pathToId,
 *   - kick a fetch if the cache doesn't already hold that folder,
 *   - populate state->maps[] from whatever's currently cached
 *     (synchronous return; the next frame will pick up fresh data).
 *
 * Map rows are emitted with a synthetic "wbn:<id>" path which the
 * lobby's click handler parses to trigger the download/upload flow.
 * Sub-folder rows use the friendly child path so a click writes
 * that back into state->currentDir and the cycle repeats. */
void lobbyWbnMapsListProvider(MapChooserState *state,
                                const char *relPath, void *ctx) {
    (void)ctx;
    state->numMaps = 0;
    if (!relPath) relPath = "";

    /* Recursive-search branch: when the user has "Search subfolders"
     * checked and typed a non-empty query, ignore relPath entirely
     * and hit /api/v1/maps/search. Results aren't scoped to the
     * current folder (the WBN endpoint is catalogue-wide). The
     * folder cache stays intact so clearing the search restores
     * the user's place. */
    if (state->searchRecursive && state->searchFilter[0] != '\0') {
        std::string query;
        std::vector<WbnMapsEntry> results;
        bool hasResults  = false;
        bool srchInFlight = s_wbnMaps.searchFetching.load();
        std::string srchErr;
        {
            std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
            query       = s_wbnMaps.searchQuery;
            results     = s_wbnMaps.searchResults;
            hasResults  = s_wbnMaps.searchHasData;
            srchErr     = s_wbnMaps.searchError;
        }
        /* Kick a fetch if the query changed. Throttled per query
         * string — typing fast doesn't pile up requests. */
        static std::string s_wbnSearchLastReq;
        if (!srchInFlight &&
            (!hasResults || query != state->searchFilter) &&
            s_wbnSearchLastReq != state->searchFilter) {
            s_wbnSearchLastReq = state->searchFilter;
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[WBN-TAB] provider kicking search for '%s'",
                state->searchFilter);
            wbnMapsKickSearchFetch(state->searchFilter);
        } else if (hasResults && query == state->searchFilter) {
            s_wbnSearchLastReq.clear();
        }

        /* Throttled log so we can confirm the branch is running
         * and inspect the first few search hits for folder info. */
        static int s_wbnSrchLogTick = 0;
        if (++s_wbnSrchLogTick % 60 == 1) {
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[WBN-TAB] search branch filter='%s' recursive=%d "
                "cached='%s' has=%d inFlight=%d hits=%zu err='%s'",
                state->searchFilter,
                (int)state->searchRecursive,
                query.c_str(), (int)hasResults, (int)srchInFlight,
                results.size(), srchErr.c_str());
            for (size_t i = 0; i < results.size() && i < 4; i++) {
                WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-TAB]   hit[%zu] id=%d folder=(id=%d name='%s') name='%s'",
                    i, results[i].id, results[i].folderId,
                    results[i].folderName.c_str(),
                    results[i].name.c_str());
            }
        }

        /* Only show results when they match the current query —
         * stale results from a previous query would be misleading. */
        if (hasResults && query == state->searchFilter) {
            for (const auto &m : results) {
                if (state->numMaps >= MAP_CHOOSER_MAX_MAPS) break;
                MapChooserEntry *e = &state->maps[state->numMaps++];
                memset(e, 0, sizeof(*e));
                SDL_strlcpy(e->name, m.name.c_str(), sizeof(e->name));
                SDL_snprintf(e->path, sizeof(e->path), "wbn:%d", m.id);
                e->isFolder = false;
                e->modTime  = m.uploadedNs;
                /* /maps/search responses now include the full path
                 * hierarchy for each hit (see wbnMapsParseSearchJson)
                 * so the enclosing-folder breadcrumb is available
                 * synchronously — no per-row /maps/info round-trip. */
                if (!m.folderName.empty()) {
                    SDL_strlcpy(e->crumbsPath, m.folderName.c_str(),
                                sizeof(e->crumbsPath));
                }
            }
        }
        /* Roll up the unique enclosing folders into folder rows pinned
         * at the top of the list, matching the Server Maps / Upload
         * tabs. Empty prefix because the WBN tab's currentDir is the
         * same friendly-path scheme crumbsPath already uses. */
        mapChooserEmitFolderRowsForSearchHits(state, "");
        /* Re-anchor by selectedPath — the rebuild + folder-reorder
         * shuffles row indices, and a stale selectedIdx would point
         * the breadcrumb at the wrong row's crumbsPath. */
        if (state->selectedPath[0] != '\0') {
            for (int i = 0; i < state->numMaps; i++) {
                if (SDL_strcmp(state->maps[i].path,
                               state->selectedPath) == 0) {
                    state->selectedIdx = i;
                    break;
                }
            }
        }
        return;
    }

    /* Resolve target folder ID and snapshot current cache. */
    int targetId = 0;
    int cachedId = -1;
    std::string cachedPath;
    std::vector<WbnMapsFolder> subs;
    std::vector<WbnMapsEntry>  entries;
    bool hasData = false;
    std::string errMsg;
    {
        std::lock_guard<std::mutex> lk(s_wbnMaps.mutex);
        auto it = s_wbnMaps.pathToId.find(relPath);
        if (it != s_wbnMaps.pathToId.end()) targetId = it->second;
        cachedId   = s_wbnMaps.currentFolderId;
        cachedPath = s_wbnMaps.currentPath;
        subs       = s_wbnMaps.subfolders;
        entries    = s_wbnMaps.entries;
        hasData    = s_wbnMaps.hasData;
        errMsg     = s_wbnMaps.error;
    }

    /* Kick a fetch if the cache doesn't match what the chooser
     * currently wants. Tracks the most recently-requested ID so
     * rapid folder clicks while one is in flight don't pile up. */
    static int s_wbnMapsLastRequested = -2;
    bool cacheMatches = hasData && (cachedPath == relPath);
    bool inFlight = s_wbnMaps.fetching.load();
    static int s_wbnLogTick = 0;
    if (++s_wbnLogTick % 60 == 1) {
        WB_LOG_INFO(WB_LOG_CAT_GUI,
            "[WBN-TAB] provider relPath='%s' targetId=%d "
            "cached=(id=%d path='%s' has=%d) match=%d inFlight=%d "
            "lastReq=%d subs=%zu entries=%zu err='%s'",
            relPath, targetId,
            cachedId, cachedPath.c_str(), (int)hasData,
            (int)cacheMatches, (int)inFlight,
            s_wbnMapsLastRequested,
            subs.size(), entries.size(),
            errMsg.c_str());
    }
    if (!cacheMatches && !inFlight) {
        if (s_wbnMapsLastRequested != targetId) {
            s_wbnMapsLastRequested = targetId;
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[WBN-TAB] provider kicking fetch for folderId=%d "
                "(relPath='%s')", targetId, relPath);
            wbnMapsKickFolderFetch(targetId);
        }
    } else if (cacheMatches) {
        s_wbnMapsLastRequested = -2;
    }

    /* Synthetic "[..]" entry — points at the parent friendly path.
     * Skipped at the root (empty relPath). The chooser hides this
     * row automatically when searchRecursive is on. */
    if (relPath[0] != '\0' && state->numMaps < MAP_CHOOSER_MAX_MAPS) {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, "[..]", sizeof(e->name));
        const char *lastSep = nullptr;
        for (const char *p = relPath; *p; p++) {
            if (*p == '/' || *p == '\\') lastSep = p;
        }
        if (lastSep) {
            size_t plen = (size_t)(lastSep - relPath);
            if (plen >= sizeof(e->path)) plen = sizeof(e->path) - 1;
            memcpy(e->path, relPath, plen);
            e->path[plen] = '\0';
        } else {
            e->path[0] = '\0';
        }
        e->isFolder   = true;
        e->isParentUp = true;
    }

    /* Only surface entries from a matching cache, else the user
     * would see the previous folder's contents while the new fetch
     * is in flight (visually identical to "no entries yet"). */
    if (!cacheMatches) return;

    /* Sub-folders first, then maps. Normally we filter empty
     * folders (count == 0) so users don't follow dead ends — but
     * the WBN API currently returns 0 for every folder, so if
     * every count is zero treat the field as unreliable and show
     * them all. Once the API returns real counts the filter takes
     * over automatically. */
    bool anyCountKnown = false;
    for (const auto &f : subs) {
        if (f.count > 0) { anyCountKnown = true; break; }
    }
    /* At the WBN root, pin folders 60 ("ClassicMap's Maps") and 61
     * ("classics-popular") to the top so the curated entry points
     * the community uses most are immediately visible. Anywhere
     * deeper the API's natural order is preserved. */
    auto isPinnedAtRoot = [&](int fid) {
        if (relPath[0] != '\0') return false;
        return fid == 60 || fid == 61;
    };
    auto emitSubfolderRow = [&](const WbnMapsFolder &f) {
        if (state->numMaps >= MAP_CHOOSER_MAX_MAPS) return;
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, f.name.c_str(), sizeof(e->name));
        if (relPath[0] != '\0') {
            SDL_snprintf(e->path, sizeof(e->path), "%s/%s",
                         relPath, f.name.c_str());
        } else {
            SDL_strlcpy(e->path, f.name.c_str(), sizeof(e->path));
        }
        e->isFolder    = true;
        e->highlighted = isPinnedAtRoot(f.id);
    };
    /* First pass: pinned folders. */
    for (const auto &f : subs) {
        if (anyCountKnown && f.count <= 0) continue;
        if (!isPinnedAtRoot(f.id)) continue;
        emitSubfolderRow(f);
    }
    /* Second pass: everything else, original order. */
    for (const auto &f : subs) {
        if (anyCountKnown && f.count <= 0) continue;
        if (isPinnedAtRoot(f.id)) continue;
        if (state->numMaps >= MAP_CHOOSER_MAX_MAPS) break;
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, f.name.c_str(), sizeof(e->name));
        if (relPath[0] != '\0') {
            SDL_snprintf(e->path, sizeof(e->path), "%s/%s",
                         relPath, f.name.c_str());
        } else {
            SDL_strlcpy(e->path, f.name.c_str(), sizeof(e->path));
        }
        e->isFolder = true;
    }
    for (const auto &m : entries) {
        if (state->numMaps >= MAP_CHOOSER_MAX_MAPS) break;
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        SDL_strlcpy(e->name, m.name.c_str(), sizeof(e->name));
        SDL_snprintf(e->path, sizeof(e->path), "wbn:%d", m.id);
        e->isFolder = false;
        e->modTime  = m.uploadedNs;
        /* crumbsPath = the current folder. ent.path is the opaque
         * "wbn:<id>" handle so the breadcrumb's path-fallback skips
         * it; without this, the preview-side breadcrumb would either
         * display only the root label or, worse, the persistent
         * activeCrumbsPath from a previous folder. */
        if (relPath && relPath[0] != '\0') {
            SDL_strlcpy(e->crumbsPath, relPath, sizeof(e->crumbsPath));
        }
    }

    /* The WBN tab re-runs this provider every frame to pick up async
     * cache updates, so maps[] is rebuilt under the chooser's feet.
     * selectedIdx is a positional index — when the array re-lays
     * out (search results landing, folder rows reordering to the
     * top), it can end up pointing at a different row, and the
     * preview-breadcrumb code would read that wrong row's
     * crumbsPath. Re-anchor by selectedPath so the row identity
     * follows the user's click instead of its array slot. */
    if (state->selectedPath[0] != '\0') {
        for (int i = 0; i < state->numMaps; i++) {
            if (SDL_strcmp(state->maps[i].path,
                           state->selectedPath) == 0) {
                state->selectedIdx = i;
                break;
            }
        }
    }
}

/* onSelect for the WBN provider. Synthetic "wbn:<id>" paths only —
 * anything else is a folder click the chooser handled internally.
 * Submitting the id kicks the SP-side download worker (which also
 * routes through MP upload when the host is a network server). */
void lobbyWbnMapsOnSelect(MapChooserState *state, void *ctx) {
    ClientSim *cs = (ClientSim *)ctx;
    if (!cs) return;
    const char *sel = state->selectedPath;
    static const char kPrefix[] = "wbn:";
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[MAPPICK] wbn onSelect sel='%s' sp=%d",
                sel ? sel : "(null)",
                (int)clientSimIsSinglePlayer(cs));
    if (!sel || strncmp(sel, kPrefix, sizeof(kPrefix) - 1) != 0) return;
    uint32_t mapId = (uint32_t)SDL_atoi(sel + sizeof(kPrefix) - 1);
    if (mapId > 0) {
        clientSimSetLobbyWbnPreviewStatus(cs, 1);
        spWbnSubmit(mapId);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[MAPPICK] wbn submitted mapId=%u previewStatus=1",
                    (unsigned)mapId);
    } else {
        WB_LOG_WARN(WB_LOG_CAT_GUI,
                    "[MAPPICK] wbn parse failed for '%s'", sel);
    }
}

/* onFolderJump for the WBN provider. The chooser prepends "Maps" as
 * the clickable root indicator. "Maps" alone (or "Maps/") jumps back
 * to the WBN catalogue root; anything else is a friendly path the
 * provider resolves via s_wbnMaps.pathToId on the next frame. */
void lobbyWbnMapsOnFolderJump(MapChooserState *state,
                                      const char *jumpPath, void *ctx) {
    (void)ctx;
    const char *jp = jumpPath;
    if (SDL_strncmp(jp, "Maps/", 5) == 0) {
        jp += 5;
    } else if (SDL_strcasecmp(jp, "Maps") == 0) {
        jp = "";
    }
    SDL_strlcpy(state->currentDir, jp, sizeof(state->currentDir));
}

/* generatePreview for the WBN provider. Runs on the chooser's
 * preview-cache worker thread. Hits /api/v1/maps/img/<id>, decodes
 * the response (any format stbi understands — PNG today) and hands
 * the RGBA pixels off to the cache. */
bool lobbyWbnGeneratePreview(const char *entryPath,
                                     MapPreviewPixels *outBuf,
                                     void *ctx) {
    (void)ctx;
    if (!entryPath || !outBuf) return false;
    static const char kPrefix[] = "wbn:";
    if (SDL_strncmp(entryPath, kPrefix, sizeof(kPrefix) - 1) != 0) {
        return false;
    }
    int mapId = SDL_atoi(entryPath + sizeof(kPrefix) - 1);
    if (mapId <= 0) return false;
    char apiPath[64];
    SDL_snprintf(apiPath, sizeof(apiPath), "maps/img/%d", mapId);
    uint8_t *imgBytes = NULL;
    size_t   imgLen   = 0;
    int status = wbn_api_download_to_memory(apiPath, &imgBytes, &imgLen);
    if (status != 200 || !imgBytes || imgLen == 0) {
        if (imgBytes) free(imgBytes);
        return false;
    }
    int w = 0, h = 0, ch = 0;
    uint8_t *decoded = stbi_load_from_memory(imgBytes,
                                              (int)imgLen,
                                              &w, &h, &ch, 4);
    free(imgBytes);
    if (!decoded || w <= 0 || h <= 0) {
        if (decoded) stbi_image_free(decoded);
        return false;
    }
    /* Copy into an SDL_malloc'd buffer so the cache can SDL_free it. */
    size_t bufSz = (size_t)w * (size_t)h * 4;
    uint8_t *pixels = (uint8_t *)SDL_malloc(bufSz);
    if (!pixels) {
        stbi_image_free(decoded);
        return false;
    }
    memcpy(pixels, decoded, bufSz);
    stbi_image_free(decoded);
    outBuf->w      = w;
    outBuf->h      = h;
    outBuf->pixels = pixels;
    return true;
}

/* WBN tick: drain any completed background download. Always called
 * on the UI thread; SP applies bytes in-process, MP feeds them into
 * the regular MAP_UPLOAD protocol. */
void lobbyWbnMapsTick(MapChooserState *state, SDL_Renderer *renderer,
                              void *ctx) {
    (void)state;
    ClientSim *cs = (ClientSim *)ctx;
    if (cs) spWbnPoll(cs, renderer);
}

/* Tooltip on the WBN path label = the WBN host URL so the user can
 * tell at a glance which server the catalogue is coming from. */
void lobbyWbnMapsTooltipPrefix(MapChooserState *state, void *ctx) {
    (void)ctx;
    const char *wbnHost = httpGetBaseUrl();
    SDL_strlcpy(state->pathTooltipPrefix,
                (wbnHost && *wbnHost) ? wbnHost : "winbolo.net",
                sizeof(state->pathTooltipPrefix));
}
#endif /* __EMSCRIPTEN__ — WBN Maps tab support code */

/* Shared per-tab render helper. Manages the chooser-area size,
 * refreshes the tooltip prefix, runs a per-frame enumerate when the
 * provider asks for it, hands the widget the supplied render slot,
 * drains breadcrumb-click jumps, and renders the provider's status
 * footer. The tab body is reduced to BeginTabItem / call / EndTabItem. */
/* Clears the chooser's selection + preview state. Used when the user
 * switches tabs (each tab has its own state, and the preview from a
 * prior visit is misleading after switching back) and on folder-jump
 * resets. */
static void lobbyMapTabClearSelection(MapChooserState *state) {
    state->selectedIdx         = -1;
    state->selectedPath[0]     = '\0';
    state->selectedName[0]     = '\0';
    state->activeCrumbsPath[0] = '\0';
    if (state->previewTex) {
        SDL_DestroyTexture(state->previewTex);
        state->previewTex = NULL;
    }
    state->previewPills  = 0;
    state->previewBases  = 0;
    state->previewStarts = 0;
}

static void lobbyRenderMapTab(MapChooserState *state, SDL_Renderer *renderer,
                               float s) {
    if (state->provider.tick) {
        state->provider.tick(state, renderer, state->provider.ctx);
    }
    if (state->provider.refreshTooltipPrefix) {
        state->provider.refreshTooltipPrefix(state, state->provider.ctx);
    }

    /* Chooser fills the entire body region. Any per-tab error
     * message is drawn via the foreground draw list (see
     * lobbyGetActiveTabError) anchored over the preview area, so the
     * body can sit flush against the action bar without leaving a
     * reservation gap for an inline footer. */
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;
    if (availH < 120.0f) availH = 120.0f;

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
        /* Refresh the listing for the new folder. WBN re-runs the
         * provider each frame so this is a no-op there, but Server
         * Maps and Upload only refresh on user actions — without this
         * the row list would stay on the previous folder until the
         * user clicked or typed. */
        mapChooserRefresh(state);
    }

}

/* Returns a short error message for the active tab (NULL if none).
 * The chooser body overlays the result top-right of the preview via
 * the foreground draw list, so a wrapped message can't push the
 * action bar out of the window. */
static const char *lobbyGetActiveTabError(ClientSim *cs) {
    if (!cs) return NULL;
    switch (s_chooserTabs.activeTab) {
        case 1: /* Local upload */
            if (!clientSimIsSinglePlayer(cs) && clientSimHasTransport(cs) &&
                clientSimGetLobbyMapUploadStatus(cs) == 4) {
                switch (clientSimGetLobbyMapUploadRejectCode(cs)) {
                    case 4: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_INFLIGHT);
                    case 5: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_DISABLED);
                    case 6: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_FULL);
                    case 7: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_COOLDOWN);
                    default: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_REJECTED);
                }
            }
            return NULL;
        case 3: /* WBN */
            if (clientSimGetLobbyWbnPreviewStatus(cs) == 3) {
                const char *m = clientSimGetLobbyWbnPreviewErrMsg(cs);
                return (m && *m) ? m : langGetText(STR_DLGLOBBY_WBN_ERR_DOWNLOAD);
            }
            if (!clientSimIsSinglePlayer(cs) && clientSimHasTransport(cs) &&
                clientSimGetLobbyMapUploadStatus(cs) == 4) {
                switch (clientSimGetLobbyMapUploadRejectCode(cs)) {
                    case 4: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_INFLIGHT);
                    case 5: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_DISABLED);
                    case 6: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_FULL);
                    case 7: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_COOLDOWN);
                    default: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_REJECTED);
                }
            }
            return NULL;
        default:
            return NULL;
    }
}

static void lobbyChooseMapEnsureInit(SDL_Renderer *renderer) {
    if (!s_chooserTabs.inited) {
        mapChooserInit(&s_chooserTabs.server, renderer);
        /* Let the chooser draw the maximize toggle overlay on the
         * preview image — it knows where the image actually lives,
         * which the surrounding lobby code doesn't.
         *
         * All four instances point at the one shared flag, wired once
         * here. s_chooser is a file-scope static, so the address is
         * stable for the life of the process: lobbyChooserReset()
         * assigns a fresh value through the object rather than
         * replacing it, and the pointer stays valid across teardown. */
        s_chooserTabs.server.maximizePtr = &s_chooser.maximized;
        /* Server Maps provider: list comes from the server. */
        s_chooserTabs.server.provider.enumerate            = lobbyServerMapsListProvider;
        s_chooserTabs.server.provider.onSelect             = lobbyServerMapsOnSelect;
        s_chooserTabs.server.provider.onFolderJump         = lobbyServerMapsOnFolderJump;
        s_chooserTabs.server.provider.refreshTooltipPrefix = lobbyServerMapsTooltipPrefix;
        s_chooserTabs.server.provider.generatePreview      = lobbyServerMapsGeneratePreview;
        s_chooserTabs.server.provider.cacheScope           = "server";
        s_chooserTabs.server.provider.ctx                  = s_chooser.cs;
        /* Network clients fetch the map list over the wire via
         * PACKET_LOBBY_MAP_LIST_REQ; the response lands asynchronously
         * after lobbyChooseMapEnsureInit's one-shot discoverMaps has
         * already returned an empty list. Without refreshEveryFrame
         * the chooser never re-polls the cache and shows nothing for
         * the lifetime of the dialog. The provider's own in-flight
         * gate prevents request spam. SP-host's synchronous branch
         * pays a no-op per-frame discoverMaps; the in-process scan
         * is already cheap so leave the flag on unconditionally. */
        s_chooserTabs.server.provider.refreshEveryFrame    = true;
        SDL_strlcpy(s_chooserTabs.server.crumbsRootLabel, "Maps",
                    sizeof(s_chooserTabs.server.crumbsRootLabel));
        /* Upload provider: local-filesystem scan via the chooser's
         * built-in helper. */
        mapChooserInit(&s_chooserTabs.upload, renderer);
        s_chooserTabs.upload.maximizePtr = &s_chooser.maximized;
        s_chooserTabs.upload.provider.enumerate            = mapChooserLocalFsEnumerate;
        s_chooserTabs.upload.provider.onSelect             = lobbyUploadOnSelect;
        s_chooserTabs.upload.provider.onFolderJump         = lobbyUploadOnFolderJump;
        s_chooserTabs.upload.provider.refreshTooltipPrefix = lobbyUploadTooltipPrefix;
        s_chooserTabs.upload.provider.generatePreview      = lobbyUploadGeneratePreview;
        s_chooserTabs.upload.provider.cacheScope           = "upload";
        SDL_strlcpy(s_chooserTabs.upload.crumbsRootLabel, "Maps",
                    sizeof(s_chooserTabs.upload.crumbsRootLabel));
        /* "Load from device" + "Generate Random Map" exist as dedicated
         * tabs in this window, so suppress the in-widget buttons that
         * would duplicate them. */
        s_chooserTabs.server.hideExtras       = true;
        s_chooserTabs.upload.hideExtras = true;
        /* ...but the local/upload tab still offers a single-file picker
         * at the top of its list, so the host can grab a .map straight
         * off disk. In multiplayer the whole tab is gated on uploads
         * being enabled, so the button only appears when it can act. */
        s_chooserTabs.upload.showDeviceLoad = true;
        /* Random tab — third chooser instance, runs in randomTabOnly
         * mode so the widget renders generator controls on the left
         * and the procedural preview on the right. */
        mapChooserInit(&s_chooserTabs.random, renderer);
        s_chooserTabs.random.maximizePtr   = &s_chooser.maximized;
        s_chooserTabs.random.hideExtras    = true;
        s_chooserTabs.random.randomTabOnly = true;
        s_chooserTabs.randomLastSeq             = 0;
        /* Keep the map list narrow so the preview can claim most of
         * the row width. ~300 px fits a column of names comfortably
         * without crowding the preview. The Random tab uses a wider
         * left panel because the generator controls need more room
         * than a single column of names. */
        s_chooserTabs.server.leftPanelMaxW        = 300.0f;
        s_chooserTabs.upload.leftPanelMaxW  = 300.0f;
        s_chooserTabs.random.leftPanelMaxW  = 360.0f;
        /* WBN provider — walks the WBN HTTP catalogue. refreshEveryFrame
         * because the listing lands asynchronously on a worker thread;
         * the tab needs to surface cache updates without user action.
         * Absent in the WASM build (no WBN HTTP backend). */
#ifndef __EMSCRIPTEN__
        mapChooserInit(&s_chooserTabs.wbn, renderer);
        s_chooserTabs.wbn.maximizePtr      = &s_chooser.maximized;
        s_chooserTabs.wbn.hideExtras       = true;
        s_chooserTabs.wbn.leftPanelMaxW    = 300.0f;
        s_chooserTabs.wbn.provider.enumerate            = lobbyWbnMapsListProvider;
        s_chooserTabs.wbn.provider.onSelect             = lobbyWbnMapsOnSelect;
        s_chooserTabs.wbn.provider.onFolderJump         = lobbyWbnMapsOnFolderJump;
        s_chooserTabs.wbn.provider.refreshTooltipPrefix = lobbyWbnMapsTooltipPrefix;
        s_chooserTabs.wbn.provider.tick                 = lobbyWbnMapsTick;
        s_chooserTabs.wbn.provider.generatePreview      = lobbyWbnGeneratePreview;
        s_chooserTabs.wbn.provider.cacheScope           = "wbn";
        s_chooserTabs.wbn.provider.refreshEveryFrame    = true;
        SDL_strlcpy(s_chooserTabs.wbn.crumbsRootLabel, "Maps",
                    sizeof(s_chooserTabs.wbn.crumbsRootLabel));
#endif /* __EMSCRIPTEN__ */
        /* Force an initial discover for each provider — mapChooserInit
         * ran discoverMaps before the providers were wired, so the
         * states landed empty. */
        s_chooserTabs.server.currentDir[0]       = '\0';
        s_chooserTabs.upload.currentDir[0] = '\0';
        mapChooserRefresh(&s_chooserTabs.server);
        mapChooserRefresh(&s_chooserTabs.upload);
        /* Land the selection on Everard if it's still entry 0. */
        if (s_chooserTabs.server.numMaps > 0) {
            s_chooserTabs.server.selectedIdx = 0;
            SDL_strlcpy(s_chooserTabs.server.selectedPath,
                        s_chooserTabs.server.maps[0].path,
                        sizeof(s_chooserTabs.server.selectedPath));
            SDL_strlcpy(s_chooserTabs.server.selectedName,
                        s_chooserTabs.server.maps[0].name,
                        sizeof(s_chooserTabs.server.selectedName));
        }
        s_chooserTabs.inited = true;
    }
}

void lobbyChooseMapOpen(ClientSim *cs, SDL_Renderer *renderer) {
    /* Cache the cs for providers before EnsureInit so the first
     * synchronous discover sees the network ctx. */
    s_chooser.cs = cs;
    lobbyChooseMapEnsureInit(renderer);
    /* Refresh each provider's cs ctx every time the chooser opens —
     * EnsureInit only runs once, but cs rebinds across game sessions.
     * All three providers use cs as ctx in their onSelect path. */
    s_chooserTabs.server.provider.ctx        = cs;
    s_chooserTabs.upload.provider.ctx  = cs;
#ifndef __EMSCRIPTEN__
    s_chooserTabs.wbn.provider.ctx     = cs;
#endif
    /* Snapshot the currently active map so Cancel can restore it
     * once an undo packet exists. */
    const char *cur = cs ? clientSimGetMapName(cs) : "";
    SDL_strlcpy(s_chooser.prevName, cur ? cur : "",
                sizeof(s_chooser.prevName));

    /* Default the chooser's highlighted entry to whatever map is
     * currently active — so SP (which loads Everard by default) opens
     * with "Everard Island (Inbuilt)" pre-selected, and subsequent
     * opens reflect any choice made last time. Falls back to entry 0
     * (Everard) if the current map isn't in the list. */
    int matchedIdx = 0;
    if (cur && cur[0] != '\0') {
        for (int i = 0; i < s_chooserTabs.server.numMaps; i++) {
            if (SDL_strcasecmp(s_chooserTabs.server.maps[i].name, cur) == 0) {
                matchedIdx = i;
                break;
            }
        }
    }
    if (matchedIdx != s_chooserTabs.server.selectedIdx) {
        s_chooserTabs.server.selectedIdx = matchedIdx;
        SDL_strlcpy(s_chooserTabs.server.selectedPath,
                    s_chooserTabs.server.maps[matchedIdx].path,
                    sizeof(s_chooserTabs.server.selectedPath));
        SDL_strlcpy(s_chooserTabs.server.selectedName,
                    s_chooserTabs.server.maps[matchedIdx].name,
                    sizeof(s_chooserTabs.server.selectedName));
        s_chooserTabs.server.randomMapSelected = false;
    }

    s_chooser.open = true;
    s_chooser.previewPending   = false;
    s_chooser.wantCloseConfirm = false;
}

/* Maximized chooser — separate ImGui window with its own ID so its
 * (small amount of) state lives independently of the normal one. X
 * here just un-maximizes (sets s_chooser.maximized = false). The
 * normal window is hidden while maximized is up. */
static void lobbyChooseMapRenderMaximizedWindow(ClientSim *cs,
                                                 SDL_Renderer *renderer,
                                                 float s,
                                                 int screenW, int screenH) {
    (void)cs;
    const float kEdgeGutter      = 15.0f;
    const float kMaxBottomGutter = 150.0f;
    float maxW = (float)screenW - kEdgeGutter * 2.0f;
    float maxH = (float)screenH - kEdgeGutter - kMaxBottomGutter;
    if (maxW < 480.0f * s) maxW = 480.0f * s;
    if (maxH < 320.0f * s) maxH = 320.0f * s;
    /* Pinned each frame so OS-window resizes are followed. */
    ImGui::SetNextWindowSize(ImVec2(maxW, maxH), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(kEdgeGutter, kEdgeGutter),
                            ImGuiCond_Always);

    bool open = true;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoScrollWithMouse
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove;
    char titleBufMax[64];
    SDL_snprintf(titleBufMax, sizeof(titleBufMax), "%s##LobbyMapChooserMax",
                 langGetText(STR_DLGLOBBY_CHOOSEMAP_TITLE));
    bool visible = ImGui::Begin(titleBufMax, &open, flags);
    /* X closes the maximized window → restore the normal one. The
     * dialog remains open the whole time. */
    if (!open) {
        s_chooser.maximized = false;
    }
    if (!visible) {
        ImGui::End();
        return;
    }

    /* Esc — same effect as the X. */
    if (ImGui::IsWindowFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s_chooser.maximized = false;
    }

    /* Mirror the visible tab — without this we'd always show the
     * server-list chooser's map even when the user was previewing
     * a generated map on the Generate tab. */
    MapChooserState *activeChooser = &s_chooserTabs.server;
    if (s_chooserTabs.activeTab == 1)      activeChooser = &s_chooserTabs.upload;
    else if (s_chooserTabs.activeTab == 2) activeChooser = &s_chooserTabs.random;
#ifndef __EMSCRIPTEN__
    else if (s_chooserTabs.activeTab == 3) activeChooser = &s_chooserTabs.wbn;
#endif

    if (activeChooser->previewView &&
        mapPreviewViewIsReady(activeChooser->previewView)) {
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y;
        if (availW < 64.0f) availW = 64.0f;
        if (availH < 64.0f) availH = 64.0f;
        mapPreviewViewRenderOffscreen(activeChooser->previewView,
                                       renderer,
                                       (int)availW, (int)availH);
        SDL_Texture *tex = mapPreviewViewGetTexture(activeChooser->previewView);
        if (tex) {
            ImVec2 imgPos = ImGui::GetCursorScreenPos();
            bool nearest =
                mapPreviewViewWantsNearestSampling(activeChooser->previewView);
            if (nearest) imguiPushNearestSampling();
            ImGui::Image((ImTextureID)tex, ImVec2(availW, availH));
            if (nearest) imguiPopNearestSampling();
            ImGui::SetCursorScreenPos(imgPos);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##MapPreviewDragMax",
                                    ImVec2(availW, availH));
            bool hovered = ImGui::IsItemHovered();
            MapPreviewInputOpts opts = { true, true, true, false };
            mapPreviewViewHandleInput(activeChooser->previewView,
                                       hovered, &opts);

            /* Zoom indicator — bottom-right corner. */
            char zoomText[16];
            SDL_snprintf(zoomText, sizeof(zoomText), "%.2fx",
                         mapPreviewViewGetZoom(activeChooser->previewView));
            ImVec2 textSize = ImGui::CalcTextSize(zoomText);
            float pad = 6.0f;
            ImVec2 textPos(imgPos.x + availW - textSize.x - pad,
                           imgPos.y + availH - textSize.y - pad);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 bgMin(textPos.x - 4.0f, textPos.y - 2.0f);
            ImVec2 bgMax(textPos.x + textSize.x + 4.0f,
                         textPos.y + textSize.y + 2.0f);
            dl->AddRectFilled(bgMin, bgMax,
                              IM_COL32(0, 0, 0, 160), 4.0f);
            dl->AddText(textPos,
                        IM_COL32(255, 255, 255, 220), zoomText);
        }
    }

    ImGui::End();
}

void lobbyChooseMapRenderWindow(ClientSim *cs, SDL_Renderer *renderer,
                                       float s, int screenW, int screenH) {
    /* Stop the preview worker on the close edge — any of the six
     * paths that flip s_chooser.open to false land here on the next
     * frame, and the worker auto-restarts on the next preview request
     * if the user reopens the chooser. */
    static bool s_prevOpen = false;
    if (s_prevOpen && !s_chooser.open) {
        mapChooserStopPreviewWorker();
    }
    s_prevOpen = s_chooser.open;
    if (!s_chooser.open) return;
    lobbyChooseMapEnsureInit(renderer);

    /* Maximized lives in its own ImGui window with a different ID so
     * its size/position are independent of the normal window's. Both
     * windows just hide/show (no destroy/recreate) — ImGui retains
     * per-window state across frames as long as nothing calls Begin
     * with the same ID, and our windowmask covers exactly one each
     * frame. */
    if (s_chooser.maximized) {
        lobbyChooseMapRenderMaximizedWindow(cs, renderer, s,
                                             screenW, screenH);
        return;
    }

    const float kEdgeGutter = 15.0f;
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    float winX = kEdgeGutter;
    float winY = kEdgeGutter;
    float winW = (float)screenW - kEdgeGutter * 2.0f;
    /* Default bottom ends ~3 lines short of the lobby's bottom edge
     * so the chooser doesn't crowd the chat / ready footer behind it.
     * (Saved settings only apply on first open; user resizes persist.) */
    float winH = (float)screenH - kEdgeGutter * 2.0f - lineH * 3.0f;
    if (winW < 480.0f * s) winW = 480.0f * s;
    if (winH < 320.0f * s) winH = 320.0f * s;
    if (winH > (float)screenH * 0.85f) winH = (float)screenH * 0.85f;
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(winX, winY),  ImGuiCond_FirstUseEver);

    /* Clamp the window inside the SDL window on every frame so a
     * lobby resize (or zoom-out via OS window controls) yanks the
     * chooser back inside. ImGui persists the chooser's pos/size
     * across frames (NoSavedSettings keeps it in-process only) —
     * we capture them after Begin and on the NEXT frame, if the
     * bottom-right corner now overflows the lobby, apply a clamped
     * SetNextWindowPos with ImGuiCond_Always so the corner snaps
     * back inside, as close to where the user had dragged it as
     * possible. Same idea SDL window managers use for "off-screen
     * recovery". */
    /* Master toggle for the bottom-right corner clamp. When false
     * the chooser is allowed to extend past the lobby's bottom-right
     * (only the top-left is held inside the gutter, so the title bar
     * can always be grabbed). User asked for this behind a flag so
     * the previous "snap fully inside on resize" behaviour can be
     * re-enabled by flipping it back to true. */
    static const bool kClampBottomRightInside = false;

    static ImVec2 s_lastWinPos(-1.0f, -1.0f);
    static ImVec2 s_lastWinSize(0.0f, 0.0f);
    if (s_lastWinPos.x >= 0.0f && s_lastWinSize.x > 0.0f) {
        /* Step 1 — clamp POSITION so the top-left stays inside the
         * gutter. Always on: keeps the title bar grabbable even if
         * the lobby shrinks past where the chooser is sitting.
         *
         * When kClampBottomRightInside is OFF the maxX/maxY caps
         * are skipped so the window may extend past the lobby's
         * bottom-right; only the < kEdgeGutter floor remains. */
        ImVec2 newPos = s_lastWinPos;
        if (kClampBottomRightInside) {
            float maxX = (float)screenW  - s_lastWinSize.x - kEdgeGutter;
            float maxY = (float)screenH  - s_lastWinSize.y - kEdgeGutter;
            if (maxX < kEdgeGutter) maxX = kEdgeGutter;
            if (maxY < kEdgeGutter) maxY = kEdgeGutter;
            if (newPos.x > maxX) newPos.x = maxX;
            if (newPos.y > maxY) newPos.y = maxY;
        }
        if (newPos.x < kEdgeGutter) newPos.x = kEdgeGutter;
        if (newPos.y < kEdgeGutter) newPos.y = kEdgeGutter;
        if (newPos.x != s_lastWinPos.x || newPos.y != s_lastWinPos.y) {
            ImGui::SetNextWindowPos(newPos, ImGuiCond_Always);
        }

        /* Step 2 — clamp SIZE so the bottom-right corner doesn't
         * extend past the lobby. Gated entirely on
         * kClampBottomRightInside. */
        if (kClampBottomRightInside) {
            ImVec2 newSize = s_lastWinSize;
            float maxRight  = (float)screenW - kEdgeGutter - newPos.x;
            float maxBottom = (float)screenH - kEdgeGutter - newPos.y;
            if (newSize.x > maxRight)  newSize.x = maxRight;
            if (newSize.y > maxBottom) newSize.y = maxBottom;
            if (newSize.x < 480.0f * s) newSize.x = 480.0f * s;
            if (newSize.y < 320.0f * s) newSize.y = 320.0f * s;
            if (newSize.x != s_lastWinSize.x ||
                newSize.y != s_lastWinSize.y) {
                ImGui::SetNextWindowSize(newSize, ImGuiCond_Always);
            }
        }
    }
    /* Min size keeps the action bar visible; no max — user can drag
     * the window as large as they like, even past the screen edges. */
    ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f * s, 320.0f * s),
                                        ImVec2(FLT_MAX, FLT_MAX));

    bool open = s_chooser.open;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoScrollWithMouse;
    char titleBuf[64];
    SDL_snprintf(titleBuf, sizeof(titleBuf), "%s##LobbyMapChooser",
                 langGetText(STR_DLGLOBBY_CHOOSEMAP_TITLE));
    if (!ImGui::Begin(titleBuf, &open, flags)) {
        ImGui::End();
        if (!open) s_chooser.open = false;
        return;
    }
    /* Snapshot the window's current rect for the next-frame clamp
     * above. ImGui::GetWindowPos / Size are only valid between Begin
     * and End. */
    s_lastWinPos  = ImGui::GetWindowPos();
    s_lastWinSize = ImGui::GetWindowSize();

    /* Action bar height (Separator + button row) — reserved AT THE
     * WINDOW LEVEL via a child container around the tab bar so the
     * tab content fills its area down to its own bottom border. The
     * old approach subtracted btnBarH inside each tab item, which
     * left a gap of dead space between the chooser's bottom border
     * and the buttons. */
    float btnBarH = ImGui::GetFrameHeight() + 14.0f * s;
    ImGui::BeginChild("##MapChooserBody",
                      ImVec2(0.0f, -btnBarH),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_NoScrollbar);

    /* Tab switch detection. Clearing the new tab's preview on entry
     * stops the prior visit's stale selection from showing through. */
    static int s_lastActiveTab = -1;
    int activeTabBefore = s_chooserTabs.activeTab;
    /* When the server is in our own process (SP, or LAN/internet host
     * binding to a local ServerSim), "Server Maps" and "Upload" both
     * read from data/maps/ — they're the same directory by definition.
     * Hide the Server Maps tab and rename the Upload tab to "Local
     * Maps" to make that visible to the user.  Remote MP clients
     * still see both: their server lives on a different machine, so
     * its data/maps/ tree is distinct from the client's. */
    bool inProcessServer = (gameFrontGetServerSim() != NULL);
    /* ImGui tab headers aren't reachable by arrow nav, so in controller mode
     * the triggers (LT/RT via the Steam menu-tab actions, or a native pad's
     * L1/R1) cycle the source tabs. Step only over the tabs visible this
     * frame so a hidden Server tab is skipped. The lobby's own tab-cycle is
     * suppressed while this chooser is open, so the trigger press is ours. */
    if (uiShouldUseControllerMode()) {
        int shift = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0)
                  - (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? 1 : 0);
        if (shift == 0)
            shift = imguiSteamNavConsumeMenuTabShift();
        if (shift != 0) {
            int vis[4];
            int nVis = 0;
            if (!inProcessServer) vis[nVis++] = 0;                  /* Server */
            if (inProcessServer ||
                clientSimGetUploadPolicy(cs) != UPLOAD_POLICY_OFF)
                vis[nVis++] = 1;                                    /* Local/Upload */
            vis[nVis++] = 2;                                        /* Generate */
#ifndef __EMSCRIPTEN__
            vis[nVis++] = 3;                                        /* WBN */
#endif
            int cur = 0;
            for (int i = 0; i < nVis; i++) {
                if (vis[i] == s_chooserTabs.activeTab) { cur = i; break; }
            }
            s_chooser.forceTab = vis[(cur + shift + nVis) % nVis];
        }
    }
    if (ImGui::BeginTabBar("##MapChooserTabs", ImGuiTabBarFlags_None)) {
        if (!inProcessServer && ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_TAB_SERVERMAPS), nullptr,
                s_chooser.forceTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooserTabs.activeTab = 0;
            if (s_lastActiveTab != 0 && activeTabBefore != 0) {
                lobbyMapTabClearSelection(&s_chooserTabs.server);
            }
            /* Drain any streamed MAP_PREVIEW bytes into the preview pane
             * (MP only; SP serves previews synchronously). */
            lobbyServerMapsPumpPreview(cs, renderer);
            lobbyRenderMapTab(&s_chooserTabs.server, renderer, s);
            ImGui::EndTabItem();
        }
        if ((inProcessServer || clientSimGetUploadPolicy(cs) != UPLOAD_POLICY_OFF) &&
            ImGui::BeginTabItem(langGetText(inProcessServer ? STR_DLGLOBBY_TAB_LOCALMAPS : STR_DLGLOBBY_TAB_UPLOAD), nullptr,
                s_chooser.forceTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooserTabs.activeTab = 1;
            if (s_lastActiveTab != 1 && activeTabBefore != 1) {
                lobbyMapTabClearSelection(&s_chooserTabs.upload);
            }
            lobbyRenderMapTab(&s_chooserTabs.upload, renderer, s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_TAB_GENERATE), nullptr,
                s_chooser.forceTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooserTabs.activeTab = 2;
            float availW = ImGui::GetContentRegionAvail().x;
            float availH = ImGui::GetContentRegionAvail().y;
            if (availH < 120.0f) availH = 120.0f;
            /* Renders generator controls on the left (where the
             * other tabs show their file list) and the procedural
             * preview on the right via the chooser's randomTabOnly
             * code path. Every config change increments genSeq —
             * we forward that to the server below so other clients
             * see the live preview. */
            mapChooserRender(&s_chooserTabs.random, renderer,
                             availW, availH, s);
            if (cs && s_chooserTabs.random.genSeq != s_chooserTabs.randomLastSeq) {
                s_chooserTabs.randomLastSeq = s_chooserTabs.random.genSeq;
                /* selectedPath is "randommap:<seed>" — strip the
                 * prefix to get the bare seed string the server
                 * expects in PACKET_LOBBY_PREVIEW_RANDOM. */
                const char *path = s_chooserTabs.random.selectedPath;
                const char *seedStr = path;
                static const char kPrefix[] = "randommap:";
                if (strncmp(path, kPrefix, sizeof(kPrefix) - 1) == 0) {
                    seedStr = path + sizeof(kPrefix) - 1;
                }
                if (clientSimIsSinglePlayer(cs)) {
                    ServerSim *sim = gameFrontGetSinglePlayerServerSim();
                    bool ok = false;
                    if (sim) {
                        threadsWaitForMutex();
                        ok = serverSimReloadRandomMap(sim,
                                &s_chooserTabs.random.genConfig);
                        threadsReleaseMutex();
                    }
                    if (ok) {
                        s_chooser.previewPending = true;
                    }
                } else {
                    clientSimNetSendLobbyPreviewRandom(cs, seedStr);
                    s_chooser.previewPending = true;
                }
            }
            ImGui::EndTabItem();
        }
#ifndef __EMSCRIPTEN__
        if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_TAB_WBNMAPS), nullptr,
                s_chooser.forceTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooserTabs.activeTab = 3;
            if (s_lastActiveTab != 3 && activeTabBefore != 3) {
                lobbyMapTabClearSelection(&s_chooserTabs.wbn);
            }
            lobbyRenderMapTab(&s_chooserTabs.wbn, renderer, s);

            ImGui::EndTabItem();
        }
#endif /* __EMSCRIPTEN__ — WBN Maps source tab */
        ImGui::EndTabBar();
    }
    /* One-shot: the forced selection has been applied (or the bar wasn't
     * drawn this frame), so don't keep re-forcing it. */
    s_chooser.forceTab = -1;
    s_lastActiveTab = s_chooserTabs.activeTab;
    ImGui::EndChild(); /* ##MapChooserBody */
    /* The child window becomes the "last item" after EndChild — its
     * screen rect is what we want to anchor the error overlay to. */
    ImVec2 chooserBodyMin = ImGui::GetItemRectMin();
    ImVec2 chooserBodyMax = ImGui::GetItemRectMax();

    /* Error overlay — anchored top-right of the chooser body (over the
     * map preview's empty header area). Old behaviour rendered the
     * footer text below the body, which pushed the Cancel / Use This
     * Map buttons past the window bottom when the message wrapped. */
    const char *errMsg = lobbyGetActiveTabError(cs);
    if (errMsg && *errMsg) {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        float pad = 8.0f * s;
        const float maxW =
            (chooserBodyMax.x - chooserBodyMin.x) * 0.6f - pad * 3.0f;
        ImFont *font = ImGui::GetFont();
        float   fsz  = ImGui::GetFontSize();
        ImVec2  textSz = font->CalcTextSizeA(fsz, FLT_MAX, maxW, errMsg);

        ImVec2 boxMin(chooserBodyMax.x - textSz.x - pad * 2.0f,
                      chooserBodyMin.y + pad * 0.5f);
        ImVec2 boxMax(chooserBodyMax.x - pad * 0.5f,
                      boxMin.y + textSz.y + pad);

        fg->AddRectFilled(boxMin, boxMax,
                          IM_COL32(40, 0, 0, 200), 4.0f * s);
        fg->AddText(font, fsz,
                    ImVec2(boxMin.x + pad, boxMin.y + pad * 0.5f),
                    IM_COL32(230, 130, 130, 255), errMsg,
                    nullptr, maxW);
    }

    /* Action bar: Cancel rolls back the server's preview; Set Map
     * commits it. Selecting any row already pushed the map to the
     * server (see Server Maps / Upload tabs above), so by the time
     * the user reaches this row the lobby is already showing the
     * previewed map on every client.
     *   - Cancel  → PACKET_LOBBY_PREVIEW_CANCEL: server reverts to
     *               the stashed prior committed map.
     *   - Set Map → PACKET_LOBBY_PREVIEW_COMMIT: server just frees
     *               the stash; nothing else needs to change. */
    ImGui::Separator();
    {
        const char *cancelLbl = langGetText(STR_DLGLOBBY_CANCEL_MAPCHOOSER);
        const char *setLbl    = langGetText(STR_DLGLOBBY_USETHISMAP);
        float wCancel = ImGui::CalcTextSize(cancelLbl).x
                      + ImGui::GetStyle().FramePadding.x * 2.0f;
        float wSet    = ImGui::CalcTextSize(setLbl).x
                      + ImGui::GetStyle().FramePadding.x * 2.0f;
        float gap     = ImGui::GetStyle().ItemSpacing.x;
        float total   = wCancel + gap + wSet;
        float startX  = (ImGui::GetContentRegionAvail().x - total) * 0.5f;
        if (startX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + startX);
        if (ImGui::Button(cancelLbl)) {
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[MAPPICK] action-bar Cancel sp=%d previewPending=%d",
                        cs ? (int)clientSimIsSinglePlayer(cs) : -1,
                        (int)s_chooser.previewPending);
            if (cs) {
                if (clientSimIsSinglePlayer(cs)) {
                    ServerSim *sim = gameFrontGetSinglePlayerServerSim();
                    if (sim) {
                        threadsWaitForMutex();
                        serverSimRevertPreview(sim);
                        threadsReleaseMutex();
                    }
                } else {
                    clientSimNetSendLobbyPreviewCancel(cs);
                }
            }
            s_chooser.previewPending = false;
            s_chooser.open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(setLbl)) {
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[MAPPICK] action-bar UseThisMap sp=%d previewPending=%d",
                        cs ? (int)clientSimIsSinglePlayer(cs) : -1,
                        (int)s_chooser.previewPending);
            if (cs) {
                if (clientSimIsSinglePlayer(cs)) {
                    ServerSim *sim = gameFrontGetSinglePlayerServerSim();
                    if (sim) {
                        threadsWaitForMutex();
                        serverSimCommitPreview(sim);
                        threadsReleaseMutex();
                    }
                } else {
                    clientSimNetSendLobbyPreviewCommit(cs);
                }
            }
            s_chooser.previewPending = false;
            s_chooser.open = false;
        }
    }

    /* Title-bar X (or Esc / Ctrl+W / Cmd+.). If the user fired at
     * least one live preview since opening the chooser, route through
     * a 3-way confirmation instead of silently reverting — they've
     * been showing this map on every client and may well want to keep
     * it. With no pending preview, the close path silently reverts.
     * CancelKeyPressed self-gates on window focus so the keypress
     * won't fire here when the preview-close confirmation popup below
     * is open over the chooser. */
    /* Esc (and controller B, which arrives as an injected Escape) closes
     * the chooser whenever it or a child is focused. The shared
     * CancelKeyPressed() defers the close while nav is inside a sub-region
     * so B/Esc first pops out of the map list to the tabs — but that only
     * makes sense when an item is actually focused (navId != 0). On reopen
     * ImGui parks focus on the body child window with navId 0 and nothing
     * to pop out to, which trapped Esc forever. So defer to the pop-out
     * only when a list item is genuinely focused (preserving controller
     * list nav); otherwise close. */
    ImGuiContext *gc = ImGui::GetCurrentContext();
    bool navOnItemInList = gc && gc->NavId != 0 &&
                           dialogNavWasInsideSubRegionAtFrameStart();
    bool escClose = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::IsKeyPressed(ImGuiKey_Escape) && !navOnItemInList;
    bool wantClose = !open || escClose;
    if (!wantClose && WBUI::CancelKeyPressed()) {  /* Ctrl+W / Cmd+. */
        wantClose = true;
    }
    if (wantClose) {
        if (s_chooser.previewPending) {
            s_chooser.wantCloseConfirm = true;
        } else {
            s_chooser.open = false;
        }
    }

    /* Confirmation popup — opened from the X-close path above and
     * also from Esc handling (same flag drives both). Three exits:
     *  - Use This Map → commit preview, close chooser.
     *  - Cancel       → revert preview to the pre-open map, close.
     *  - Keep Picking → dismiss the popup, keep the chooser open. */
    if (s_chooser.wantCloseConfirm) {
        ImGui::OpenPopup("##MapPreviewCloseConfirm");
        s_chooser.wantCloseConfirm = false;
    }
    static bool s_mpccOpen = true; s_mpccOpen = true;
    if (ImGui::BeginPopupModal("##MapPreviewCloseConfirm", &s_mpccOpen,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_CLOSEMAP_PROMPT));
        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_CLOSEMAP_QUESTION));
        /* [Cancel: Keep picking] [Destructive: Revert and Close] [Primary: Use This Map]. */
        int f = WBUI::DialogFooter3(langGetText(STR_DLGLOBBY_KEEPPICKING),
                                    langGetText(STR_DLGLOBBY_REVERTCLOSE),
                                    langGetText(STR_DLGLOBBY_USETHISMAP));
        if (f == WBUI::FOOTER_CONFIRM) {
            if (cs) {
                if (clientSimIsSinglePlayer(cs)) {
                    ServerSim *sim = gameFrontGetSinglePlayerServerSim();
                    if (sim) {
                        threadsWaitForMutex();
                        serverSimCommitPreview(sim);
                        threadsReleaseMutex();
                    }
                } else {
                    clientSimNetSendLobbyPreviewCommit(cs);
                }
            }
            s_chooser.previewPending = false;
            s_chooser.open           = false;
            ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_DESTRUCTIVE) {
            if (cs) {
                if (clientSimIsSinglePlayer(cs)) {
                    ServerSim *sim = gameFrontGetSinglePlayerServerSim();
                    if (sim) {
                        threadsWaitForMutex();
                        serverSimRevertPreview(sim);
                        threadsReleaseMutex();
                    }
                } else {
                    clientSimNetSendLobbyPreviewCancel(cs);
                }
            }
            s_chooser.previewPending = false;
            s_chooser.open           = false;
            ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_CANCEL) {
            /* Re-open the chooser window — Begin's `open` flag was
             * flipped false when the user hit X, so without this
             * we'd close on the very next frame. */
            s_chooser.open = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

#if !BOLO_MOBILE
/* ── Embedded replay reel ─────────────────────────────────────────
 * The log viewer decodes on its own timer threads and paints the round
 * into an SDL render target; the recap blits the visible slice of that
 * target as an image and feeds wheel/drag input back. The viewer owns a
 * process-wide decoder singleton, so the reel is torn down whenever the
 * recap stops drawing it or the summary clears. */

static LobbyReelState s_reel = {};

/* Core reads what is playing, recap the slack the reel hands back, clipgif
 * the crop frame and the last blitted slice. */
LobbyReelState *lobbyReel(void) {
    return &s_reel;
}

#if BOLO_RECAP_CLIP_GIF
/* Smallest crop the frame will shrink to, in displayed pixels. */
static const float REEL_CROP_MIN_PX = 16.0f;
/* Grab margin either side of an edge, in displayed pixels. */
static const float REEL_CROP_GRAB_PX = 6.0f;
#endif

/* A backward scrub cannot continue the decode — it has to restore the round's
 * snapshot and replay from there — so only the newest one in a burst is paid
 * for. Forward scrubs are incremental and run every frame. */
#define RECAP_SCRUB_BACK_MS 90

#if BOLO_REEL_WBN_FETCH
/* Where a round shared through WinBolo.net comes from. A host registered with
 * WinBolo.net uploads its log there rather than serving it over the game
 * connection, so a client that joined fetches the same bytes over HTTP using
 * the key the round summary carries. The upload lands a few seconds after the
 * recap opens, so the first GET is expected to miss a round that is about to
 * exist; a bounded ladder covers that lag without becoming a request a frame.
 *
 * Under the curl transport the worker writes buf/len/status and then releases
 * s_reelWbn.done; the render thread reads those three only after acquiring it,
 * which is the whole hand-off — one producer, one consumer, no mutex. Every
 * other field here is the render thread's own, s_reelWbn.running included, so
 * the guards that decide whether to start another attempt never read a field a
 * worker is writing. The fetch transport has no second thread at all and fills
 * the same three fields from its poll. */
static const int    REEL_WBN_RETRY_MAX = 12;
static const Uint64 REEL_WBN_RETRY_MS  = 5000;

typedef struct LobbyReelWbnFetch {
    char                   key[ROUND_STATS_LOGKEY_LEN] = "";
#if BOLO_REEL_WBN_FETCH_CURL
    /* The worker and the two fields that coordinate with it. Nothing outside
     * the curl transport has a second thread to coordinate with. */
    std::thread            thread;
    std::atomic<bool>      done{false};
    volatile int           cancel = 0;
#endif
    /* What the transfer has moved, for the reading below. Curl fills both from
     * its worker as bytes arrive; fetch resolves the whole body at once and
     * leaves them at zero, which that reading already treats as "nothing to
     * report yet". */
    std::atomic<long long> bytesNow{0};
    std::atomic<long long> bytesTotal{0};
    bool                   running   = false;
    uint8_t               *buf       = nullptr;
    size_t                 len       = 0;
    int                    status    = 0;
    int                    attempts  = 0;
    Uint64                 retryAtMs = 0;
} LobbyReelWbnFetch;

/* Cleared a field at a time by lobbyReelWbnAbort and never as a whole: the
 * worker handle and the two counters are not assignable, and the thread has to
 * be cancelled and joined before any of the rest may be touched. */
static LobbyReelWbnFetch s_reelWbn;

/* Stop whatever is in flight and forget the round it belonged to. The curl
 * worker is joined and never detached: it writes into the fields above, and a
 * lobby that has gone away leaves nothing for it to write into. Cancelling
 * first is what keeps the join short, since curl polls the flag as bytes
 * arrive. The fetch transport aborts its request and drops the slot its reply
 * would have landed in, which is the same thing without the join. */
static void lobbyReelWbnAbort(void) {
#if BOLO_REEL_WBN_FETCH_CURL
    s_reelWbn.cancel = 1;
    if (s_reelWbn.thread.joinable()) s_reelWbn.thread.join();
#else
    wbRoundLogFetchCancel();
#endif
    free(s_reelWbn.buf);
    s_reelWbn.buf       = nullptr;
    s_reelWbn.len       = 0;
    s_reelWbn.status    = 0;
    s_reelWbn.running   = false;
    s_reelWbn.attempts  = 0;
    s_reelWbn.retryAtMs = 0;
    s_reelWbn.key[0]    = '\0';
    s_reelWbn.bytesNow.store(0, std::memory_order_relaxed);
    s_reelWbn.bytesTotal.store(0, std::memory_order_relaxed);
#if BOLO_REEL_WBN_FETCH_CURL
    s_reelWbn.done.store(false, std::memory_order_relaxed);
    s_reelWbn.cancel = 0;
#endif
}

/* Spend one rung of the ladder, if one is due. Everything that would make a
 * fetch pointless is a guard rather than a condition at the call site, so the
 * caller can ask every frame. */
static void lobbyReelWbnKick(void) {
    if (s_reelWbn.running || s_reelWbn.buf) return;
    if (s_reelWbn.key[0] == '\0') return;
    /* The key is pasted into the request path below, so it never goes out
     * unless it is the 32-hex shape WBN issues. The codec already drops a
     * malformed one off the wire; this is the backstop on the path itself,
     * the same one wbn_comments_fetch_start applies to its own. */
    if (!winbolonetKeyIsValid(s_reelWbn.key)) return;
    /* The load below gets one attempt per summary; once it has spent it there
     * is nothing left to play another copy of the same round. */
    if (s_reel.tried) return;
    if (s_reelWbn.attempts >= REEL_WBN_RETRY_MAX) return;
    if (SDL_GetTicks() < s_reelWbn.retryAtMs) return;
#if BOLO_REEL_WBN_FETCH_CURL
    /* A worker that finished without the poll below seeing it still owns a
     * thread handle; std::thread destructs hard on a joinable one. */
    if (s_reelWbn.thread.joinable()) s_reelWbn.thread.join();

    /* The one WinBolo.net entry point that does not bring HTTP up on its own:
     * it fails outright when nothing has called httpCreate, where the GET and
     * POST paths create lazily. Reentrant, so the browser's own create/destroy
     * pair is unaffected. Once a round is enough. */
    if (s_reelWbn.attempts == 0) httpCreate();
#endif

    char keyCopy[ROUND_STATS_LOGKEY_LEN];
    SDL_strlcpy(keyCopy, s_reelWbn.key, sizeof(keyCopy));

#if BOLO_REEL_WBN_FETCH_CURL
    /* Curl's byte sink, running on the worker. Non-capturing so it converts to
     * the C function pointer, and it keeps the last total it was given, since
     * curl reports zero until it has read a Content-Length. */
    WbnProgressFn progressFn = [](void *user, int64_t now, int64_t total) {
        (void)user;
        s_reelWbn.bytesNow.store((long long)now, std::memory_order_relaxed);
        if (total > 0) {
            s_reelWbn.bytesTotal.store((long long)total, std::memory_order_relaxed);
        }
    };
#endif

    s_reelWbn.attempts++;
    s_reelWbn.running = true;
    s_reelWbn.status  = 0;
#if BOLO_REEL_WBN_FETCH_CURL
    s_reelWbn.done.store(false, std::memory_order_relaxed);
#endif
    s_reelWbn.bytesNow.store(0, std::memory_order_relaxed);
    s_reelWbn.bytesTotal.store(0, std::memory_order_relaxed);
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[REEL] winbolo.net round log attempt %d/%d, key prefix '%.6s'",
                s_reelWbn.attempts, REEL_WBN_RETRY_MAX, keyCopy);

#if !BOLO_REEL_WBN_FETCH_CURL
    /* Returns at once; the reply lands in the page and the poll below takes it
     * on a later frame. The ceiling is applied there, where the bytes are. */
    wbRoundLogFetchStart(keyCopy);
#else
    s_reelWbn.thread = std::thread([keyCopy, progressFn]() {
        char path[128];
        SDL_snprintf(path, sizeof(path), "logs/%s/download", keyCopy);
        uint8_t *data = nullptr;
        size_t   size = 0;
        /* Held to the same ceiling the server-served path enforces on its own
         * transfer. Without it this is the one way into the reel that a round
         * log of any size at all can come through, and the recap opens and
         * fetches on its own between rounds. */
        int status = wbn_api_download_to_memory_progress(path, &data, &size,
                                                         ROUND_LOG_MAX_BYTES,
                                                         progressFn, nullptr,
                                                         &s_reelWbn.cancel);
        if (status != 200 || size == 0) {
            free(data);
            data = nullptr;
            size = 0;
        }
        s_reelWbn.buf    = data;
        s_reelWbn.len    = size;
        s_reelWbn.status = status;
        /* Last, and releasing: the three writes above are published by it. */
        s_reelWbn.done.store(true, std::memory_order_release);
    });
#endif
}

/* Collect a finished attempt. A 200 leaves its bytes standing for the load
 * below to take; anything else arms the next rung. */
static void lobbyReelWbnPoll(void) {
#if BOLO_REEL_WBN_FETCH_CURL
    if (!s_reelWbn.done.load(std::memory_order_acquire)) return;
    if (s_reelWbn.thread.joinable()) s_reelWbn.thread.join();
    s_reelWbn.done.store(false, std::memory_order_relaxed);
#else
    /* Still in flight reads as 0; any other answer settles the attempt, and
     * brings the bytes with it when there are any to bring. */
    uint8_t *fetched = nullptr;
    int      fetchedLen = 0;
    int      fetchedStatus = wbRoundLogFetchPoll(&fetched, &fetchedLen);
    if (fetchedStatus == 0) return;
    s_reelWbn.buf    = fetched;
    s_reelWbn.len    = (fetched != nullptr) ? (size_t)fetchedLen : 0;
    s_reelWbn.status = fetchedStatus;
#endif
    s_reelWbn.running = false;

    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[REEL] winbolo.net round log attempt %d done: status %d, "
                "%zu bytes",
                s_reelWbn.attempts, s_reelWbn.status, s_reelWbn.len);
    if (s_reelWbn.buf) return;

    s_reelWbn.len       = 0;
    s_reelWbn.retryAtMs = SDL_GetTicks() + REEL_WBN_RETRY_MS;
    if (s_reelWbn.attempts >= REEL_WBN_RETRY_MAX) {
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[REEL] winbolo.net round log gave up after %d attempts",
                    s_reelWbn.attempts);
    }
}
#endif /* BOLO_REEL_WBN_FETCH */

#if BOLO_RECAP_CLIP_GIF
/* Defined with the clip export below, which needs the reel's own state. */
void lobbyClipGifReset(void);
#endif

void lobbyReelEnd(void) {
#if BOLO_RECAP_CLIP_GIF
    /* An export in flight is holding encoder allocations and a playback
     * position to put back, and the reel it was reading is about to go. What
     * the capture held goes back to its declared values with it. */
    lobbyClipGifReset();
#endif
#if BOLO_REEL_WBN_FETCH
    /* And a WinBolo.net fetch is a thread writing into state this is about to
     * zero, so it is cancelled and joined here too. */
    lobbyReelWbnAbort();
#endif
    /* The viewer owns a process-wide decoder singleton, so it is torn down
     * before the state that says a reel is up is overwritten. */
    if (s_reel.active) {
        lvEmbedEnd();
    }
    /* Everything else goes back to its declared value in one write. The next
     * summary asks for its own round's log and reports nothing about a
     * transfer until it has one, and a crop frame belongs to the clip it was
     * drawn for rather than to the session. */
    s_reel = LobbyReelState{};
}

/* Trace the transfer as it moves, so winbolo.log tells a slow download apart
 * from a stalled one and both apart from a refusal — the recap draws nothing
 * about it yet. Ten-percent steps while bytes arrive keep a 4 MB transfer to a
 * handful of lines. */
static void lobbyReelLogTransfer(int state, uint8_t percent) {
    const int step = (state == CLIENT_ROUND_LOG_DOWNLOADING) ? percent / 10 : -1;
    if (state == s_reel.logStateSeen && step == s_reel.logStepSeen) return;
    s_reel.logStateSeen = state;
    s_reel.logStepSeen  = step;
    switch (state) {
        case CLIENT_ROUND_LOG_WAITING:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log requested, no bytes yet");
            break;
        case CLIENT_ROUND_LOG_DOWNLOADING:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log downloading %u%%", (unsigned)percent);
            break;
        case CLIENT_ROUND_LOG_READY:
            WB_LOG_INFO(WB_LOG_CAT_GUI, "[REEL] round log ready to play");
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log unavailable: server does not serve logs");
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_NONE:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log unavailable: server has no completed round");
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE:
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[REEL] round log unavailable: over the transfer cap");
            break;
        default:
            /* Idle: nothing asked for, or the viewer has taken the blob. */
            break;
    }
}

/* Give back a downloaded round that nothing is going to play. A blob belongs
 * to the round its recap described, and the transport holds a completed one
 * until it is taken — so once the summary is gone (the countdown clears it)
 * the bytes are stale and the next round must fetch its own. Kept apart from
 * lobbyReelEnd, which also runs at the end of a lobby session with no
 * ClientSim in reach. */
void lobbyReelDropRoundLog(ClientSim *cs) {
    if (clientSimGetRoundLogState(cs) != CLIENT_ROUND_LOG_READY) return;
    size_t len = 0;
    uint8_t *buf = clientSimTakeRoundLog(cs, &len);
    free(buf);
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[REEL] dropped round log (%zu bytes), its recap is gone", len);
}

/* Say where the round log has got to, centred in the space the reel would have
 * filled, so a recap with nothing to play yet reads as waiting or refused
 * rather than as a blank rectangle.
 *
 * Waiting and each of the three refusals get an icon and a line of their own —
 * the refusals are worded apart because they tell a player different things:
 * one is the host's choice, one is a round the server never finished, one is a
 * round too long to send. A transfer in flight gets a ring struck from the
 * percent the transport reports, which is a real fraction of the bytes and not
 * an animation, so a stalled download looks stalled.
 *
 * Reports only: the state is whatever the caller read off the transport this
 * frame, and nothing here times anything out, retries, or moves the transfer
 * along. */
static void lobbyRenderReelStatus(int state, uint8_t percent, ImVec2 rect,
                                  float s) {
    /* The recap can be the first thing on screen in the controller layout's
     * Last round tab, where the player list that usually brings the status
     * icons up is not drawn. The load is idempotent, so asking again is free. */
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (renderer) lobbyLoadStatusIconsOnce(renderer, s);

    const char  *msg = nullptr;
    SDL_Texture *ico = nullptr;
    char msgBuf[128];

    switch (state) {
        case CLIENT_ROUND_LOG_WAITING:
            ico = lobbyIcons()->info;
            msg = langGetText(STR_DLGLOBBY_REEL_WAITING);
            break;
        case CLIENT_ROUND_LOG_DOWNLOADING: {
            /* Copied out rather than held: langGetTextFmt returns a pointer
             * into a short ring of buffers that later calls reuse. */
            MessageArgs args = {};
            args.number = (int)percent;
            SDL_snprintf(msgBuf, sizeof(msgBuf), "%s",
                         langGetTextFmt(STR_DLGLOBBY_REEL_DOWNLOADING, &args));
            msg = msgBuf;
            break;
        }
        case CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED:
            ico = lobbyIcons()->error;
            msg = langGetText(STR_DLGLOBBY_REEL_DISABLED);
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_NONE:
            ico = lobbyIcons()->error;
            msg = langGetText(STR_DLGLOBBY_REEL_NONE);
            break;
        case CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE:
            ico = lobbyIcons()->error;
            msg = langGetText(STR_DLGLOBBY_REEL_TOO_LARGE);
            break;
        default:
            /* Idle: nothing was asked for on this connection, or the viewer has
             * already taken the bytes and a load is on its way. Neither is
             * something to tell the player about, and drawing nothing at all
             * leaves the recap the layout it had before there was a transfer to
             * describe. */
            return;
    }

    const ImGuiStyle &style = ImGui::GetStyle();
    const bool   ring     = (state == CLIENT_ROUND_LOG_DOWNLOADING);
    const float  ringR    = 22.0f * s;
    const float  iconSize = 18.0f * s;
    const float  gapX     = style.ItemSpacing.x;
    const float  gapY     = style.ItemSpacing.y;
    const ImVec2 textSz   = ImGui::CalcTextSize(msg);
    /* Icon and message share one row; the ring, when there is one, sits above
     * it and the pair is centred in the area as a block. */
    const float rowH   = (ico && iconSize > textSz.y) ? iconSize : textSz.y;
    const float rowW   = textSz.x + (ico ? iconSize + gapX : 0.0f);
    const float blockH = rowH + (ring ? ringR * 2.0f + gapY : 0.0f);

    /* A round still on its way holds the reel's whole rect, so the scoreboard
     * does not shuffle down the moment the reel appears in that same space. A
     * refusal never becomes a reel, so it keeps only the band its own line
     * needs and leaves the rest of the panel to the rows below. */
    const bool holdRect = (state == CLIENT_ROUND_LOG_WAITING || ring);
    ImVec2     area(rect.x, holdRect ? rect.y : blockH + gapY * 2.0f);
    if (area.y > rect.y) area.y = rect.y;

    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 pmin = ImGui::GetCursorScreenPos();
    const ImVec2 pmax(pmin.x + area.x, pmin.y + area.y);

    /* Nothing may leave that area — a long translation is cut off rather than
     * written across the rows underneath. */
    dl->PushClipRect(pmin, pmax, true);
    dl->AddRectFilled(pmin, pmax, ImGui::GetColorU32(ImGuiCol_FrameBg),
                      style.FrameRounding);

    const float cx  = pmin.x + area.x * 0.5f;
    const float top = pmin.y + (area.y - blockH) * 0.5f;

    if (ring) {
        /* Struck clockwise from 12 o'clock on a full-circle track, the shape
         * the vote ring already uses. */
        const ImVec2 centre(cx, top + ringR);
        dl->AddCircle(centre, ringR, ImGui::GetColorU32(ImGuiCol_TextDisabled),
                      36, 3.0f * s);
        const float frac = ((percent > 100) ? 100.0f : (float)percent) / 100.0f;
        if (frac > 0.0f) {
            const float a0 = -IM_PI * 0.5f;
            dl->PathArcTo(centre, ringR, a0, a0 + frac * IM_PI * 2.0f, 36);
            dl->PathStroke(ImGui::GetColorU32(ImGuiCol_PlotHistogram),
                           3.5f * s);
        }
    }

    const float rowY = top + blockH - rowH;
    float       x    = cx - rowW * 0.5f;
    if (ico) {
        const ImVec2 iconMin(x, rowY + (rowH - iconSize) * 0.5f);
        dl->AddImage((ImTextureID)ico, iconMin,
                     ImVec2(iconMin.x + iconSize, iconMin.y + iconSize));
        x += iconSize + gapX;
    }
    dl->AddText(ImVec2(x, rowY + (rowH - textSz.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), msg);
    dl->PopClipRect();

    /* Spend what was drawn into, so the rows below start under it and the
     * height the recap feeds back has somewhere to land instead of climbing. */
    ImGui::Dummy(area);
}

/* Reel height: a share of whatever vertical room the container has left, plus
 * the room the rest of the recap turned out not to need, floored so it stays
 * watchable in a short controller tab and capped as a share of the container
 * so the transport row underneath is never pushed off. The share is the term
 * that decides the size in practice: the slack only ever hands over room the
 * rest of the recap genuinely left, which in a panel the scoreboard already
 * overflows is none. The floor is in unscaled pixels; the cap is a fraction,
 * because a tall panel is exactly the case the slack exists to fill. Taken
 * from the content region rather than the window so the same numbers serve
 * both containers. */
static const float REEL_HEIGHT_FRAC     = 0.60f;
static const float REEL_HEIGHT_MIN      = 180.0f;
static const float REEL_HEIGHT_MAX_FRAC = 0.85f;

/* Defined down with the chat input's state, which is declared after this. */
void lobbyChatInputAppendTime(uint32_t curMs);

#if BOLO_RECAP_CLIP_GIF
/* Defined with the clip export below, which needs the reel's own state. The
 * transport bar carries the same control the clip rows do, so both are reached
 * from here. */
bool lobbyClipGifButton(const char *id, bool compact);
void lobbyClipGifStartFromPlayhead(uint32_t curMs, const char *mapName);
/* Whether an export is running. The crop frame reads it to hold still: the
 * rect is fixed once at msf_gif_begin and every frame of the GIF is that size,
 * so letting it be dragged mid-capture would show a box the recording is not
 * following. */
bool lobbyClipGifActive(void);

/* The crop control, sitting with the export it crops. Names the frame rather
 * than describing it — the same rule the GIF control's caption follows, and
 * the reason both ship a word rather than a sentence. */
static const char *const REEL_CROP_TITLE = "Crop";

/* Toggle button carrying a drawn square-frame glyph. Drawn with the draw list
 * rather than loaded, so the control needs no new art asset and follows the
 * text colour and UI scale the way the icons beside it do. Stays pressed while
 * the frame is up, which is how the file's other state buttons read. */
static bool lobbyReelCropButton(const char *id) {
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const bool  on    = s_reel.cropOn;

    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button(id, ImVec2(lineH + sty.FramePadding.x * 2.0f,
                                            lineH + sty.FramePadding.y * 2.0f));
    if (on) {
        ImGui::PopStyleColor();
    }

    /* The glyph: a square outline inset in the button, with the corners drawn
     * heavier so it reads as a crop frame rather than an empty box. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec2 a(p.x + sty.FramePadding.x + 1.0f, p.y + sty.FramePadding.y + 1.0f);
    ImVec2 b(a.x + lineH - 2.0f, a.y + lineH - 2.0f);
    dl->AddRect(a, b, col, 0.0f, 0, 1.0f);
    float tick = (b.x - a.x) * 0.32f;
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x + tick, a.y), col, 2.0f);
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.x, a.y + tick), col, 2.0f);
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x - tick, b.y), col, 2.0f);
    dl->AddLine(ImVec2(b.x, b.y), ImVec2(b.x, b.y - tick), col, 2.0f);

    imguiHelpTooltip(REEL_CROP_TITLE);
    return clicked;
}

/* Draw the crop frame over the reel and let it be moved and resized. Returns
 * true when the frame owns this frame's mouse, so the caller sits its pan out.
 *
 * Precedence: while the frame is up, its interior and its handles win over the
 * reel's drag-pan. The handles are submitted after the reel's own overlay
 * button, which is marked allow-overlap, so ImGui's hit test hands them the
 * hover; the caller additionally gates the pan on the return value so the press
 * frame cannot slip through. The wheel is left alone entirely — zooming still
 * works with the cursor anywhere over the reel, frame included.
 *
 * Everything is computed in displayed pixels and stored back normalized, and
 * every edge is clamped inside the image, so the frame can neither leave the
 * view nor invert. */
static bool lobbyReelCropOverlay(ImVec2 imgMin, ImVec2 imgSize,
                                 bool panHoldsMouse) {
    if (!s_reel.cropOn || imgSize.x < 1.0f || imgSize.y < 1.0f) {
        return false;
    }
    const bool locked = lobbyClipGifActive();

    float x0 = imgMin.x + s_reel.cropX0 * imgSize.x;
    float y0 = imgMin.y + s_reel.cropY0 * imgSize.y;
    float x1 = imgMin.x + s_reel.cropX1 * imgSize.x;
    float y1 = imgMin.y + s_reel.cropY1 * imgSize.y;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    /* Dim the frame while a capture is running: it is showing what the GIF is
     * taking, and it is not going to answer a drag. */
    ImU32 line = locked ? IM_COL32(255, 255, 255, 110) : IM_COL32(255, 255, 255, 230);
    ImU32 shade = IM_COL32(0, 0, 0, 90);
    /* Shade everything the export will drop, so the kept area reads at a
     * glance rather than having to be traced along the outline. */
    dl->AddRectFilled(imgMin, ImVec2(imgMin.x + imgSize.x, y0), shade);
    dl->AddRectFilled(ImVec2(imgMin.x, y1),
                      ImVec2(imgMin.x + imgSize.x, imgMin.y + imgSize.y), shade);
    dl->AddRectFilled(ImVec2(imgMin.x, y0), ImVec2(x0, y1), shade);
    dl->AddRectFilled(ImVec2(x1, y0),
                      ImVec2(imgMin.x + imgSize.x, y1), shade);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 160), 0.0f, 0, 3.0f);
    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), line, 0.0f, 0, 1.0f);

    if (locked) {
        return false;
    }

    /* Which part of the frame a press took hold of. Latched, so a drag that
     * wanders off the handle keeps resizing the edge it started on. */
    static int  s_grab = 0;   /* bit 1 left, 2 right, 4 top, 8 bottom, 16 move */
    static bool s_dragging = false;

    /* A pan already holding the mouse keeps it. Ownership is decided at the
     * press and not re-decided per frame, so dragging the reel across the
     * frame does not hand the drag over halfway and stall the pan — the same
     * mistake the map preview's start markers used to make. */
    if (panHoldsMouse && !s_dragging) {
        return false;
    }

    const float g = REEL_CROP_GRAB_PX;
    ImVec2 mp = ImGui::GetMousePos();
    int hot = 0;
    bool overFrame = (mp.x >= x0 - g && mp.x <= x1 + g &&
                      mp.y >= y0 - g && mp.y <= y1 + g);
    if (overFrame) {
        if (mp.x >= x0 - g && mp.x <= x0 + g) hot |= 1;
        if (mp.x >= x1 - g && mp.x <= x1 + g) hot |= 2;
        if (mp.y >= y0 - g && mp.y <= y0 + g) hot |= 4;
        if (mp.y >= y1 - g && mp.y <= y1 + g) hot |= 8;
        if (hot == 0 && mp.x > x0 && mp.x < x1 && mp.y > y0 && mp.y < y1) {
            hot = 16;
        }
    }

    /* One invisible item over the whole frame plus its grab margin, so ImGui
     * knows the press belongs here and the reel's pan button does not take it.
     * Submitted after the reel's overlay (which allows overlap), which is what
     * puts it in front for hit-testing. */
    bool takesMouse = false;
    if (hot != 0 || s_dragging) {
        ImGui::SetCursorScreenPos(ImVec2(x0 - g, y0 - g));
        ImGui::InvisibleButton("##ReelCropGrab",
                               ImVec2((x1 - x0) + g * 2.0f, (y1 - y0) + g * 2.0f));
        takesMouse = ImGui::IsItemHovered() || ImGui::IsItemActive() || s_dragging;
        if (ImGui::IsItemActivated()) {
            s_grab = hot;
            s_dragging = true;
        }
    }

    int shape = s_dragging ? s_grab : hot;
    if (shape != 0) {
        ImGuiMouseCursor cur = ImGuiMouseCursor_ResizeAll;
        switch (shape & 15) {
            case 1: case 2:            cur = ImGuiMouseCursor_ResizeEW; break;
            case 4: case 8:            cur = ImGuiMouseCursor_ResizeNS; break;
            case 1 | 4: case 2 | 8:    cur = ImGuiMouseCursor_ResizeNWSE; break;
            case 2 | 4: case 1 | 8:    cur = ImGuiMouseCursor_ResizeNESW; break;
            default:                   break;   /* move */
        }
        ImGui::SetMouseCursor(cur);
    }

    if (s_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        const float minW = REEL_CROP_MIN_PX;
        if (s_grab & 16) {
            /* Move: both edges together, stopped by the image rather than
             * squashed against it. */
            float w = x1 - x0, h = y1 - y0;
            x0 += d.x; y0 += d.y;
            if (x0 < imgMin.x) x0 = imgMin.x;
            if (y0 < imgMin.y) y0 = imgMin.y;
            if (x0 + w > imgMin.x + imgSize.x) x0 = imgMin.x + imgSize.x - w;
            if (y0 + h > imgMin.y + imgSize.y) y0 = imgMin.y + imgSize.y - h;
            x1 = x0 + w; y1 = y0 + h;
        } else {
            if (s_grab & 1) x0 += d.x;
            if (s_grab & 2) x1 += d.x;
            if (s_grab & 4) y0 += d.y;
            if (s_grab & 8) y1 += d.y;
            if (x0 < imgMin.x) x0 = imgMin.x;
            if (y0 < imgMin.y) y0 = imgMin.y;
            if (x1 > imgMin.x + imgSize.x) x1 = imgMin.x + imgSize.x;
            if (y1 > imgMin.y + imgSize.y) y1 = imgMin.y + imgSize.y;
            /* No inverting: the dragged edge stops a minimum short of its
             * opposite instead of crossing it. */
            if (x1 - x0 < minW) {
                if (s_grab & 1) x0 = x1 - minW; else x1 = x0 + minW;
            }
            if (y1 - y0 < minW) {
                if (s_grab & 4) y0 = y1 - minW; else y1 = y0 + minW;
            }
            /* The min-size correction can push an edge back out of the image
             * on a very small view; clamp once more so it never does. */
            if (x0 < imgMin.x) { x0 = imgMin.x; if (x1 < x0 + minW) x1 = x0 + minW; }
            if (y0 < imgMin.y) { y0 = imgMin.y; if (y1 < y0 + minW) y1 = y0 + minW; }
        }
        s_reel.cropX0 = (x0 - imgMin.x) / imgSize.x;
        s_reel.cropY0 = (y0 - imgMin.y) / imgSize.y;
        s_reel.cropX1 = (x1 - imgMin.x) / imgSize.x;
        s_reel.cropY1 = (y1 - imgMin.y) / imgSize.y;
    } else if (s_dragging) {
        s_dragging = false;
        s_grab = 0;
    }
    return takesMouse;
}
#endif

/* One zoom step per notch of travel, from either gesture a trackpad offers.
 *
 * The wheel arrives as whatever SDL had from the device: a mouse notch is a
 * clean 1.0, but a macOS trackpad scrolls with precise deltas, which SDL passes
 * on as fractions and the ImGui backend forwards unscaled. Stepping on every
 * frame that carried anything at all therefore ran the whole 0.5x..4x ladder
 * in the first few frames of a two-finger flick and sat at whichever end it
 * reached, which reads as a reel that will not zoom rather than one that zooms
 * too eagerly. Banking the deltas and spending them a whole notch at a time
 * leaves a mouse feeling exactly as it did and gives the trackpad the same
 * distance-per-step.
 *
 * Pinch is the gesture that never arrived at all: macOS sends magnification as
 * its own NSEvent and SDL does not turn it into a wheel, so the reel — alone
 * among the map views — could not answer the one gesture a laptop user reaches
 * for first. macos_pinch.m has been collecting it all along for the map
 * preview, the map editor and the standalone log viewer; the reel now spends it
 * on the same 0.15 threshold, so a pinch travels the same in all four.
 *
 * Both banks are dropped whenever the reel goes a frame without the cursor:
 * the pinch monitor is process-wide and keeps filling wherever the cursor is
 * (and the reel is not even drawn while the panel's Map tab is up), so a
 * gesture aimed at something else must not arrive here as a jump the moment
 * the reel is hovered again. Continuity is judged on the ImGui frame counter
 * rather than a hovered/not flag, which covers the frames the reel is not
 * drawn at all as well as the ones where the cursor is simply elsewhere. */
#define REEL_WHEEL_STEP 1.0f  /* a mouse notch, the unit the wheel reports in */
#define REEL_PINCH_STEP 0.15f /* magnification per step; matches the map views */

static void lobbyReelZoomInput(bool hovered, ImVec2 imgMin) {
    if (!hovered) {
        return;
    }

    /* Anything banked before a gap belongs to a gesture that has ended. */
    const int frame = ImGui::GetFrameCount();
    const bool continuing = (s_reel.zoomFrame == frame - 1);
    s_reel.zoomFrame = frame;
    if (!continuing) {
        s_reel.wheelAccum = 0.0f;
        s_reel.pinchAccum = 0.0f;
    }

    const ImVec2 mp = ImGui::GetMousePos();
    const int localX = (int)(mp.x - imgMin.x);
    const int localY = (int)(mp.y - imgMin.y);

    float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
        /* A reversal is a new gesture, not a continuation of the old one: drop
         * what the other direction had banked so turning around answers on the
         * next notch instead of paying the leftover back first. */
        if ((wheel > 0.0f) != (s_reel.wheelAccum > 0.0f)) {
            s_reel.wheelAccum = 0.0f;
        }
        s_reel.wheelAccum += wheel;
        while (s_reel.wheelAccum >= REEL_WHEEL_STEP) {
            lvEmbedWheel(localX, localY, 1.0f);
            s_reel.wheelAccum -= REEL_WHEEL_STEP;
        }
        while (s_reel.wheelAccum <= -REEL_WHEEL_STEP) {
            lvEmbedWheel(localX, localY, -1.0f);
            s_reel.wheelAccum += REEL_WHEEL_STEP;
        }
    }

    /* Consumed on every hovered frame, gap or not: the accumulator is shared
     * with the other views, so the backlog has to be taken off it either way —
     * what a gap changes is that it is thrown away rather than spent. */
    const float pinch = macOSPinchZoomConsume();
    if (pinch != 0.0f && continuing) {
        s_reel.pinchAccum += pinch;
        while (s_reel.pinchAccum >= REEL_PINCH_STEP) {
            lvEmbedWheel(localX, localY, 1.0f);
            s_reel.pinchAccum -= REEL_PINCH_STEP;
        }
        while (s_reel.pinchAccum <= -REEL_PINCH_STEP) {
            lvEmbedWheel(localX, localY, -1.0f);
            s_reel.pinchAccum += REEL_PINCH_STEP;
        }
    }
}

void lobbyRenderReel(ClientSim *cs, const RoundStatsSummary *st,
                            float s) {
    /* Source, in order: the file this process wrote, else the copy the server
     * sent us, else the copy WinBolo.net holds under the round's key.
     *
     * A triple gate on the file: only a round this process recorded, published
     * to a file that is actually there. gameFrontHasLocalServer() is the
     * load-bearing one — the accessor describes whatever round this process
     * last recorded, so a player who hosted, left and then joined someone
     * else's server would otherwise see a completely different game replayed
     * here. It does not cover a host that is local but is not the server that
     * recorded the round; what covers that is gameFrontShutdownServer clearing
     * the accessor as it tears each server down. */
    const char *replayPath = serverDedicatedLogLastRoundFile();
    SDL_PathInfo replayInfo;
    const bool haveLocalFile = gameFrontHasLocalServer() &&
                               replayPath[0] != '\0' &&
                               SDL_GetPathInfo(replayPath, &replayInfo);

    /* Sized before the source is settled, because a round still on its way from
     * the server draws its delivery state into this same rect. Nothing has been
     * drawn yet either way, so the room measured here is the room the reel gets
     * once there is one. */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 rect(avail.x, avail.y * REEL_HEIGHT_FRAC + s_reel.recapSlack);
    if (rect.y < REEL_HEIGHT_MIN * s) rect.y = REEL_HEIGHT_MIN * s;
    /* Cap last, so a container too short for the floor is still not overrun. */
    if (rect.y > avail.y * REEL_HEIGHT_MAX_FRAC) {
        rect.y = avail.y * REEL_HEIGHT_MAX_FRAC;
    }
    if (rect.y < 1.0f) rect.y = 1.0f;
    if (rect.x < 1.0f) rect.x = 1.0f;

    /* A load that came to nothing is the end of it for this round: the bytes
     * were had and refused, so there is no source left to try. Said with the
     * arm that already means "no replay for this round", and said here because
     * the block below would otherwise re-read the transfer state and re-kick
     * the WinBolo.net fetch every frame, leaving the recap on "asking the
     * server" for the rest of the lobby. */
    if (s_reel.loadFailed) {
        lobbyRenderReelStatus(CLIENT_ROUND_LOG_UNAVAILABLE_NONE, 0, rect, s);
        return;
    }

    /* A client that joined recorded nothing, so it asks the server that ran
     * the round for the bytes and waits for them. The request goes out from
     * here and nowhere else, so a player who never opens the recap never costs
     * the server a transfer, and the latch closes on a send the transport
     * accepted rather than on the attempt — the recap can be drawing before
     * the return-to-lobby handshake has finished, and a request made then goes
     * nowhere. Until it is accepted the ask is retried each frame, which costs
     * one comparison against the join state. Asking comes before reading the
     * state, which is what stops a blob left over from an earlier round being
     * played as this one: the request supersedes whatever the transport is
     * still holding. Only until a reel is up: taking the blob returns the
     * state to idle, and past that point idle means the viewer has it, not
     * that there is nothing to play.
     *
     * Until the bytes are here the rect above carries the transfer's state
     * instead of a reel. A host or single-player session that recorded the
     * round never reaches this: it resolves to its own file above and goes
     * straight to playing it, asking for nothing and drawing no overlay. */
    if (!s_reel.active && !haveLocalFile) {
        if (!s_reel.logAsked && clientSimNetSendRoundLogRequest(cs)) {
            s_reel.logAsked = true;
        }
        s_reel.logState = clientSimGetRoundLogState(cs);
        s_reel.logPercent = (s_reel.logState == CLIENT_ROUND_LOG_DOWNLOADING)
                               ? clientSimGetRoundLogPercent(cs)
                               : 0;
        lobbyReelLogTransfer(s_reel.logState, s_reel.logPercent);

        /* A host registered with WinBolo.net hands the round there instead of
         * serving it itself, so its refusal is the cue to go and get the same
         * bytes over HTTP. Only the three terminal answers qualify: a transfer
         * still moving is left alone to finish. Without a key there is nowhere
         * to go, and the refusal stands as the thing the recap says. */
        const bool serverRefused =
            s_reel.logState == CLIENT_ROUND_LOG_UNAVAILABLE_DISABLED ||
            s_reel.logState == CLIENT_ROUND_LOG_UNAVAILABLE_NONE ||
            s_reel.logState == CLIENT_ROUND_LOG_UNAVAILABLE_TOO_LARGE;
        bool haveWbnBytes = false;
        if (serverRefused && st && st->wbnLogKey[0] != '\0') {
            /* A key that is not the one being fetched belongs to a later
             * round, and whatever the previous one gathered is stale. */
            if (strncmp(s_reelWbn.key, st->wbnLogKey, sizeof(s_reelWbn.key)) != 0) {
                lobbyReelWbnAbort();
                SDL_strlcpy(s_reelWbn.key, st->wbnLogKey, sizeof(s_reelWbn.key));
            }
            lobbyReelWbnKick();
            lobbyReelWbnPoll();
            haveWbnBytes = (s_reelWbn.buf != nullptr);
            if (!haveWbnBytes) {
                /* Said in the states the overlay already draws, so the fetch
                 * costs no state of its own and no string of its own: a
                 * transfer with a length behind it is a download with a real
                 * fraction of the bytes, a spent ladder is a round with no
                 * replay to be had, and anything else is still waiting. */
                const long long got =
                    s_reelWbn.bytesNow.load(std::memory_order_relaxed);
                const long long total =
                    s_reelWbn.bytesTotal.load(std::memory_order_relaxed);
                int     wbnState = CLIENT_ROUND_LOG_WAITING;
                uint8_t wbnPct   = 0;
                if (s_reelWbn.running && total > 0) {
                    long long pct = got * 100 / total;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    wbnState = CLIENT_ROUND_LOG_DOWNLOADING;
                    wbnPct   = (uint8_t)pct;
                } else if (!s_reelWbn.running &&
                           s_reelWbn.attempts >= REEL_WBN_RETRY_MAX) {
                    wbnState = CLIENT_ROUND_LOG_UNAVAILABLE_NONE;
                }
                lobbyRenderReelStatus(wbnState, wbnPct, rect, s);
                return;
            }
        }

        if (!haveWbnBytes && s_reel.logState != CLIENT_ROUND_LOG_READY) {
            lobbyRenderReelStatus(s_reel.logState, s_reel.logPercent, rect, s);
            return;
        }
    }

    if (!s_reel.active && !s_reel.tried) {
        /* One attempt per summary either way — a failed load must not be
         * retried every frame. */
        s_reel.tried = true;
        s_reel.viewW = rect.x;
        s_reel.viewH = rect.y;
        if (haveLocalFile) {
            SDL_IOStream *io = SDL_IOFromFile(replayPath, "rb");
            if (io) {
                Sint64 len = SDL_GetIOSize(io);
                /* malloc, not SDL_malloc: the viewer releases the buffer with
                 * plain free(), and it owns it from the call on — including
                 * when the load fails. */
                uint8_t *buf = (len > 0) ? (uint8_t *)malloc((size_t)len) : NULL;
                if (buf) {
                    if (SDL_ReadIO(io, buf, (size_t)len) == (size_t)len) {
                        s_reel.active = lvEmbedBegin(sdl3DrawGetWindow(),
                                                    sdl3DrawGetRenderer(),
                                                    buf, (size_t)len,
                                                    (int)rect.x, (int)rect.y);
                    } else {
                        free(buf);
                    }
                }
                SDL_CloseIO(io);
            }
        } else {
            /* The downloaded blob is already the bytes the viewer wants, on
             * the same malloc terms as the read above — hand it straight over
             * rather than looking at it first, since the viewer owns it from
             * the call on and frees it even when it refuses the data.
             * WinBolo.net's copy goes first when there is one, since it is
             * only ever fetched after the server has already declined to send
             * one. Nulling the static as the pointer goes is what keeps the
             * ownership single: from here the viewer frees it, and nothing
             * else may. */
            size_t   len = 0;
            uint8_t *buf = nullptr;
            if (s_reelWbn.buf) {
                buf          = s_reelWbn.buf;
                len          = s_reelWbn.len;
                s_reelWbn.buf = nullptr;
                s_reelWbn.len = 0;
            } else {
                buf = clientSimTakeRoundLog(cs, &len);
            }
            if (buf) {
                s_reel.active = lvEmbedBegin(sdl3DrawGetWindow(),
                                            sdl3DrawGetRenderer(),
                                            buf, len,
                                            (int)rect.x, (int)rect.y);
            }
        }
        /* Tell a reel that came up who is watching it, so the round is drawn
         * from their side: their team's tanks green, the other side's red.
         * The lobby slot is where that lives — its name is the key the log
         * shares, and the team alliances the server applied at kickoff are in
         * the log itself, so the reel needs nothing else. A spectator has no
         * slot of their own (and myPlayerNum is a stale index for one), so
         * they are named as nobody and the reel draws as it always did. */
        const ClientLobbySlot *watcher =
            clientSimIsSpectator(cs)
                ? nullptr
                : clientSimGetLobbySlot(cs, clientSimGetMyPlayerNum(cs));
        lvEmbedSetSelfName(watcher ? watcher->playerName : "");

        /* No reel out of the attempt means there is no replay to be had for
         * this round, whichever way it fell short — the file would not open,
         * came up short, would not fit in memory, no bytes were handed over,
         * or the viewer refused the ones that were. They all read the same to
         * the player, and none of them get better by being tried again. */
        s_reel.loadFailed = !s_reel.active;
    }
    if (!s_reel.active) return;

    /* The frame-end hook pauses a reel the recap stopped drawing, which is
     * every frame the panel's Map tab is up. Drawing again undoes that pause
     * — but only when the pause was ours, so a deliberate one survives a
     * round trip through the other tab. */
    if (s_reel.autoPaused) {
        s_reel.autoPaused = false;
        lvEmbedPlay();
    }

    if (rect.x != s_reel.viewW || rect.y != s_reel.viewH) {
        lvEmbedSetViewportSize((int)rect.x, (int)rect.y);
        s_reel.viewW = rect.x;
        s_reel.viewH = rect.y;
    }

    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    ImVec2 imgMin = ImGui::GetCursorScreenPos();
    float  blockTopY = ImGui::GetCursorPosY();
    ImVec2 imgSize = rect;
    if (lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) &&
        tex && texW > 0 && texH > 0) {
        /* The tile grid is fitted to the rect by rounding to whole tiles, so
         * the slice it reports rarely lands on the rect exactly. Trim the
         * visible slice down to whole source pixels and take the drawn size
         * from that, so one source pixel is always exactly `zoom` host pixels
         * — the ratio the standalone viewer gets by blitting at that multiple
         * and letting the window edge clip. What the trim leaves over is under
         * one zoom step wide and stays as padding. */
        float zoom = lvEmbedGetZoomLevel();
        if (zoom <= 0.0f) zoom = 1.0f;
        int visW = (int)floorf(rect.x / zoom);
        int visH = (int)floorf(rect.y / zoom);
        if (visW < 1) visW = 1;
        if (visH < 1) visH = 1;
        if (visW > srcW) visW = srcW;
        if (visH > srcH) visH = srcH;
        imgSize.x = (float)visW * zoom;
        imgSize.y = (float)visH * zoom;
        /* The render target is a tile larger than the visible slice, so the
         * UVs pick whole source pixels out of it, starting at the sub-tile pan
         * offset. Whole pixels on both edges are what keeps the blit an exact
         * multiple instead of a resample. */
        ImVec2 uv0((float)srcX / (float)texW, (float)srcY / (float)texH);
        ImVec2 uv1((float)(srcX + visW) / (float)texW,
                   (float)(srcY + visH) / (float)texH);
        /* The reel is game pixel art magnified `zoom` times. Drawn through
         * ImGui the SDL_Renderer backend forces LINEAR on every texture it
         * binds, which is what made the replay look soft — the same defect the
         * lobby's inline map preview had. Point-sample it for the blit and put
         * LINEAR back for the surrounding UI. */
        imguiPushNearestSampling();
        ImGui::Image((ImTextureID)tex, imgSize, uv0, uv1);
        imguiPopNearestSampling();
#if BOLO_RECAP_CLIP_GIF
        /* Publish the exact slice this blit used. The crop frame is normalized
         * against it, so an export maps the frame back through the same
         * numbers the picture was drawn with. */
        s_reel.lastSlice.x = srcX;
        s_reel.lastSlice.y = srcY;
        s_reel.lastSlice.w = visW;
        s_reel.lastSlice.h = visH;
#endif
    } else {
        ImGui::Dummy(rect);
    }

    /* Input overlay on the image rect: the button takes the drag as an
     * active item, so a drag pans the reel instead of moving the window
     * under it. Mirrors the map preview popup. Sized to the image, not the
     * rect, so the coordinates handed back to the viewer are image-local. */
    ImGui::SetCursorScreenPos(imgMin);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##ReelView", imgSize);
    const bool reelHovered = ImGui::IsItemHovered();
    if (reelHovered) {
        /* Claim the wheel on every hovered frame, not just the ones that
         * carry a notch: the ownership set here is what ImGui reads at the
         * start of the next frame, and it is what stops the enclosing recap
         * window from scrolling under the reel as it zooms. */
        ImGui::SetKeyOwner(ImGuiKey_MouseWheelY, ImGui::GetItemID());
    }
    lobbyReelZoomInput(reelHovered, imgMin);
    const bool reelPanActivated = ImGui::IsItemActivated();
    const bool reelPanActive =
        ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    ImVec2 reelPanDrag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);

    /* The crop frame goes on after the reel's own overlay so its handles win
     * the hit test, and it reports back whether it took the mouse — the pan
     * below sits out the frames it did. */
    bool cropTookMouse = false;
#if BOLO_RECAP_CLIP_GIF
    cropTookMouse = lobbyReelCropOverlay(imgMin, imgSize, reelPanActive);
#endif

    if (!cropTookMouse) {
        if (reelPanActivated) {
            lvEmbedPanBegin();
        }
        if (reelPanActive) {
            lvEmbedPanDelta(reelPanDrag.x, reelPanDrag.y);
        }
    }

    /* Claim the whole rect whatever the image came out at, so zooming does
     * not shuffle everything below the reel up and down. */
    ImGui::SetCursorPosY(blockTopY + rect.y);

    /* Transport toggle as a glyph: a written caption is the widest thing on
     * this row and its width moves as the label swaps and as the language
     * changes, which shoves everything after it. The icon is square and the
     * two states are the same size, so the row holds still. The written label
     * stays as the tooltip — it is already translated, and a bare glyph does
     * not say what it does for someone meeting it the first time. */
    const bool reelPlaying = lvEmbedIsPlaying();
    SDL_Texture *transportIcon = reelPlaying ? lobbyIcons()->pause : lobbyIcons()->play;
    const char  *transportText = langGetText(reelPlaying ? STR_LV_PAUSE
                                                         : STR_LV_PLAY_BTN);
    bool transportClicked;
    if (transportIcon) {
        transportClicked = ImGui::ImageButton(
            "##reelplay", (ImTextureID)transportIcon,
            ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()),
            ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
            ImGui::GetStyleColorVec4(ImGuiCol_Text));
        imguiHelpTooltip(transportText);
    } else {
        transportClicked = ImGui::Button(transportText);
    }
    if (transportClicked) {
        /* Whichever way it goes, the player has now said what they want —
         * drop any claim we had on the transport. */
        s_reel.autoPaused = false;
        if (reelPlaying) {
            lvEmbedPause();
        } else {
            lvEmbedPlay();
        }
    }

    uint32_t curMs = 0, totalMs = 0;
    lvEmbedGetProgress(&curMs, &totalMs);

    /* Drop where the reel is sitting into the chat box, so the moment can be
     * talked about. Punctuation rather than a lang string, like the zoom
     * buttons: the token it writes is the caption. */
    ImGui::SameLine();
    if (ImGui::Button("@")) {
        lobbyChatInputAppendTime(curMs);
    }

#if BOLO_RECAP_CLIP_GIF
    /* Export the next few seconds from wherever the reel is sitting. The view
     * is left as the player framed it — they have already chosen what they are
     * looking at, which is the whole point of exporting from here rather than
     * off a row. Full height, so it sits level with the two buttons before
     * it rather than shrinking the transport row. */
    ImGui::SameLine();
    if (lobbyClipGifButton("##reelgif", false)) {
        lobbyClipGifStartFromPlayhead(curMs, clientSimGetMapName(cs));
    }

    /* Crop frame, next to the export it crops: with it up, that export takes
     * the framed rectangle instead of the whole visible view. */
    ImGui::SameLine();
    if (lobbyReelCropButton("##reelcrop")) {
        s_reel.cropOn = !s_reel.cropOn;
    }
#endif

    /* Seek slider shares the transport row with Play/Pause and takes the rest
     * of the width. Times are the presented window's, which with the lobby
     * hidden is the round itself. */
    ImGui::SameLine();
    if (!s_reel.seeking) {
        s_reel.seekRatio = (totalMs > 0) ? ((float)curMs / (float)totalMs) : 0.0f;
        if (s_reel.seekRatio > 1.0f) s_reel.seekRatio = 1.0f;
        /* Idle: the applied value is wherever the reel actually is, so the
         * next drag measures its first step from the truth. */
        s_reel.seekApplied = s_reel.seekRatio;
    }
    unsigned curSecs = (unsigned)(curMs / 1000u);
    char seekLabel[48];
    if (totalMs > 0) {
        unsigned totalSecs = (unsigned)(totalMs / 1000u);
        snprintf(seekLabel, sizeof(seekLabel), "%02u:%02u / %02u:%02u",
                 curSecs / 60u, curSecs % 60u, totalSecs / 60u, totalSecs % 60u);
    } else {
        snprintf(seekLabel, sizeof(seekLabel), "%02u:%02u / --:--",
                 curSecs / 60u, curSecs % 60u);
    }
    ImGui::PushItemWidth(-1);
    /* NoRoundToFormat is essential: seekLabel is a pre-rendered string
     * ("01:12 / 04:30"), not a numeric printf format. Without the flag ImGui
     * rounds the dragged value by round-tripping it through that label, which
     * parses back to 0 and pins every seek to the start of the log. */
    if (ImGui::SliderFloat("##ReelSeek", &s_reel.seekRatio, 0.0f, 1.0f, seekLabel,
                           ImGuiSliderFlags_NoRoundToFormat)) {
        if (!s_reel.seeking) {
            /* Freeze playback for the scrub instead of letting every applied
             * seek pause and resume it — lvEmbedSeekRatio does that by
             * removing and re-adding the two SDL playback timers, which is
             * not something to do sixty times a second. */
            s_reel.seeking        = true;
            s_reel.seekWasPlaying = lvEmbedIsPlaying();
            if (s_reel.seekWasPlaying) lvEmbedPause();
            s_reel.seekAppliedMs  = 0;
        }
    }
    /* Live scrub: the tanks follow the handle as it is dragged, not only when
     * it is dropped.
     *
     * The two directions cost very different amounts. A seek restores the
     * newest snapshot at or before the target and re-decodes forward to it at
     * 20 ms of log per tick, and a round carries essentially one snapshot — at
     * its start — so a naive per-frame seek re-decoded the whole round every
     * frame, which is what made this release-only. Forward seeks no longer pay
     * that: lv_screenSeekToAbsoluteMs now continues the decode from where it
     * already stands, so dragging right costs only the ticks the handle
     * crossed since the last frame and can run every frame. Dragging left
     * still has to rewind through the snapshot, so it is throttled and only
     * the newest seek in a burst is paid for.
     *
     * The release below always applies the exact dropped value, so where it
     * lands is never a throttled approximation. */
    if (s_reel.seeking && ImGui::IsItemActive()) {
        Uint64 nowMs = SDL_GetTicks();
        bool   apply = false;
        if (s_reel.seekRatio > s_reel.seekApplied) {
            apply = true;                       /* forward: incremental */
        } else if (s_reel.seekRatio < s_reel.seekApplied) {
            apply = (nowMs - s_reel.seekAppliedMs >= RECAP_SCRUB_BACK_MS);
        }
        if (apply) {
            lvEmbedSeekRatio(s_reel.seekRatio);
            s_reel.seekApplied   = s_reel.seekRatio;
            s_reel.seekAppliedMs = nowMs;
        }
    }
    /* IsItemDeactivated, not ...AfterEdit: this also has to un-pause, and a
     * release that ImGui does not count as an edit would otherwise leave the
     * reel frozen for good. */
    if (s_reel.seeking && ImGui::IsItemDeactivated()) {
        s_reel.seeking = false;
        /* Land exactly on the dropped value. A no-op when the last live apply
         * already got there — a forward seek to the current time decodes
         * nothing. */
        lvEmbedSeekRatio(s_reel.seekRatio);
        s_reel.seekApplied = s_reel.seekRatio;
        if (s_reel.seekWasPlaying) lvEmbedPlay();
        s_reel.seekWasPlaying = false;
    }
    ImGui::PopItemWidth();

    s_reel.drawn = true;
}

#if BOLO_RECAP_CLIP_GIF
/* ── Clip GIF export ──────────────────────────────────────────────
 * A clip row can hand its moment to a GIF the player can post somewhere.
 * The round is not sitting in memory as frames — it has to be replayed to be
 * seen — so the export steps the reel five ticks at a time and reads the
 * result back off the GPU, which is a second or more of work for a long clip.
 * That runs a batch per lobby frame under a modal rather than in one loop, so
 * the lobby keeps drawing and the player can call it off.
 *
 * The caption and the modal's title are the format's name, not copy — the same
 * rule the transport's @ button follows. */
static const char *const CLIP_GIF_TITLE = "GIF";
static const char *const CLIP_GIF_POPUP = "GIF##clipgif";

/* Log ticks per captured frame. The viewer's log clock is 20 ms an entry, so
 * five of them is 100 ms — 10 fps, which is 10 centiseconds a frame. This is
 * the reel's clock only; a clip's own duration is in sim ticks and is never
 * divided by this. */
static const int CLIP_GIF_TICKS_PER_FRAME = 5;
static const int CLIP_GIF_CS_PER_FRAME    = 10;
/* Wall-clock length of one captured frame, the same 10 fps said in ms. */
static const uint32_t CLIP_GIF_FRAME_MS   = 100u;
static const int CLIP_GIF_QUALITY         = 16;  /* the encoder's own default */
/* 15 s of clip, and a floor so a clip that arrives with no duration still
 * exports something rather than an empty file. */
static const int CLIP_GIF_MAX_FRAMES      = 150;
static const int CLIP_GIF_MIN_FRAMES      = 10;
/* What the transport's own button captures, having no clip to take a length
 * from: long enough to hold a moment, short enough to still be worth posting. */
static const uint32_t CLIP_GIF_PLAYHEAD_MS = 5000u;
/* Frames per lobby frame. Four keeps the longest clip under a second of
 * wall time while leaving the readback stalls small enough to hide. */
static const int CLIP_GIF_FRAMES_PER_PASS = 4;
/* Every frame in a GIF is the same size, so the crop is fixed once at the
 * start; this caps how wide it may be, and the height follows the same ratio
 * so the clip keeps the shape the reel showed. */
static const int CLIP_GIF_MAX_WIDTH       = 480;

/* One export, from the moment it is armed to the moment its bytes are handed
 * over: the encoder itself, how far through the clip it has got, the rectangle
 * every frame is read back from, where the reel has to go afterwards and what
 * the file will be called. All of it belongs to that one export, so the reset
 * below puts the lot back. */
typedef struct ClipGifCapture {
    bool        active         = false;
    bool        failed         = false;
    MsfGifState enc            = {};
    int         frame          = 0;
    int         total          = 0;
    /* inside the viewer's render target */
    SDL_Rect    crop           = { 0, 0, 0, 0 };
    uint32_t    restoreMs      = 0;  /* where the reel was before we took it */
    bool        restorePlaying = false;
    char        name[96]       = "";  /* <map>_<mmss>, the file's base name */
} ClipGifCapture;

static ClipGifCapture s_clipGif = {};

/* <map>_<mmss>, reduced to characters every filesystem here will take — a map
 * name is free text and reaches this straight off the wire. */
static void lobbyClipGifBaseName(char *out, size_t outLen, const char *mapName,
                                 unsigned mins, unsigned secs) {
    char base[64];
    if (mapName && mapName[0]) {
        snprintf(base, sizeof(base), "%s", mapName);
        size_t blen = SDL_strlen(base);
        if (blen > 4 && SDL_strcmp(base + blen - 4, ".map") == 0) {
            base[blen - 4] = '\0';
        }
    } else {
        snprintf(base, sizeof(base), "clip");
    }
    for (char *p = base; *p != '\0'; p++) {
        char c = *p;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '-' || c == '_';
        if (!keep) *p = '_';
    }
    snprintf(out, outLen, "%s_%02u%02u", base, mins, secs);
}

/* Put the reel back where the player had it, transport included: an export is
 * a detour, not a seek they asked for. */
static void lobbyClipGifRestoreReel(void) {
    lvEmbedSeekToTime(s_clipGif.restoreMs);
    if (s_clipGif.restorePlaying) {
        lvEmbedPlay();
    }
}

/* End the encoder however the capture ended — it holds heap buffers from
 * msf_gif_begin on and only msf_gif_end releases them, so cancelling has to
 * come through here too. outResult takes the finished bytes when the caller
 * means to write them (and then owes msf_gif_free); NULL throws them away. */
static void lobbyClipGifFinish(MsfGifResult *outResult) {
    MsfGifResult res = msf_gif_end(&s_clipGif.enc);
    if (outResult != NULL) {
        *outResult = res;
    } else {
        msf_gif_free(res);
    }
    lobbyClipGifRestoreReel();
    s_clipGif.active = false;
    s_clipGif.failed = false;
    s_clipGif.frame  = 0;
    s_clipGif.total  = 0;
}

static void lobbyClipGifAbort(void) {
    if (s_clipGif.active) {
        lobbyClipGifFinish(NULL);
    }
}

/* Back to the declared values, encoder included. The abort comes first and is
 * what makes the write safe: it is the only route to msf_gif_end, and the
 * buffers a live capture holds are leaked outright if they are overwritten
 * instead of ended. A capture that was never begun has nothing to end and
 * nothing to leak. */
void lobbyClipGifReset(void) {
    lobbyClipGifAbort();
    s_clipGif = ClipGifCapture{};
}

bool lobbyClipGifActive(void) {
    return s_clipGif.active;
}

/* Park the reel on the moment and open the encoder at the size every frame of
 * this capture will be. Takes a time and a length rather than a clip: the
 * transport's button has neither a clip nor a cell, only where the playhead is.
 * centreOnCell splits the two the same way lvEmbedSeekWindowMs does — a clip
 * row names a place as well as a moment, the transport names only a moment and
 * leaves the framing the player set up alone. */
static void lobbyClipGifStart(uint32_t startMs, uint32_t durationMs,
                              bool centreOnCell, int mapX, int mapY,
                              const char *mapName) {
    if (s_clipGif.active || !lvEmbedIsActive()) {
        return;
    }

    lvEmbedGetProgress(&s_clipGif.restoreMs, NULL);
    s_clipGif.restorePlaying = lvEmbedIsPlaying();
    lvEmbedPause();
    /* Same seek the caller's own control does, so the capture opens on the
     * moment it named. */
    if (centreOnCell) {
        lvEmbedSeekToClip(startMs, mapX, mapY);
    } else {
        lvEmbedSeekToTime(startMs);
    }

    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    if (!lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) ||
        tex == NULL || srcW <= 0 || srcH <= 0) {
        lobbyClipGifRestoreReel();
        return;
    }
    /* The slice is reported against a target a tile larger than itself, but
     * clamp anyway — the crop is read back as-is for every frame after this. */
    if (srcX + srcW > texW) srcW = texW - srcX;
    if (srcY + srcH > texH) srcH = texH - srcY;
    if (srcW <= 0 || srcH <= 0) {
        lobbyClipGifRestoreReel();
        return;
    }
    int cropW, cropH;
    if (s_reel.cropOn && s_reel.lastSlice.w > 0 && s_reel.lastSlice.h > 0) {
        /* The frame the player drew is what this takes. It is normalized
         * against the slice the reel blitted, so mapping it back is that same
         * slice's extent times the fractions — the identical arithmetic the
         * Image's uv0/uv1 used, which is what makes the GIF exactly the
         * rectangle the outline showed.
         *
         * The extent comes from the last draw (an on-screen size a seek cannot
         * change) while the origin is the fresh one read above, because the
         * seek this export just did may have moved the camera and the frame
         * names a place on the view, not on the map. */
        int visW = s_reel.lastSlice.w;
        int visH = s_reel.lastSlice.h;
        if (visW > srcW) visW = srcW;
        if (visH > srcH) visH = srcH;
        int fx0 = (int)(s_reel.cropX0 * (float)visW + 0.5f);
        int fy0 = (int)(s_reel.cropY0 * (float)visH + 0.5f);
        int fx1 = (int)(s_reel.cropX1 * (float)visW + 0.5f);
        int fy1 = (int)(s_reel.cropY1 * (float)visH + 0.5f);
        if (fx0 < 0) fx0 = 0;
        if (fy0 < 0) fy0 = 0;
        if (fx1 > visW) fx1 = visW;
        if (fy1 > visH) fy1 = visH;
        cropW = fx1 - fx0;
        cropH = fy1 - fy0;
        if (cropW < 1) cropW = 1;
        if (cropH < 1) cropH = 1;
        s_clipGif.crop.x = srcX + fx0;
        s_clipGif.crop.y = srcY + fy0;
        s_clipGif.crop.w = cropW;
        s_clipGif.crop.h = cropH;
        /* No CLIP_GIF_MAX_WIDTH here: that cap trims an uncropped view down to
         * something worth posting, and a frame is the player saying what to
         * take instead. */
    } else {
        cropW = srcW;
        cropH = srcH;
        if (cropW > CLIP_GIF_MAX_WIDTH) {
            cropW = CLIP_GIF_MAX_WIDTH;
            cropH = (int)((float)srcH * (float)cropW / (float)srcW);
        }
        if (cropW < 1) cropW = 1;
        if (cropH < 1) cropH = 1;
        if (cropH > srcH) cropH = srcH;
        s_clipGif.crop.x = srcX + (srcW - cropW) / 2;
        s_clipGif.crop.y = srcY + (srcH - cropH) / 2;
        s_clipGif.crop.w = cropW;
        s_clipGif.crop.h = cropH;
    }

    /* From the length in ms, not in ticks: the reel steps a log clock and a
     * clip is measured in sim ticks, and the two do not share a rate. */
    uint32_t frames = durationMs / CLIP_GIF_FRAME_MS;
    if (frames > (uint32_t)CLIP_GIF_MAX_FRAMES) frames = CLIP_GIF_MAX_FRAMES;
    if (frames < (uint32_t)CLIP_GIF_MIN_FRAMES) frames = CLIP_GIF_MIN_FRAMES;

    if (!msf_gif_begin(&s_clipGif.enc, cropW, cropH)) {
        lobbyClipGifRestoreReel();
        return;
    }

    unsigned secs = (unsigned)(startMs / 1000u);
    lobbyClipGifBaseName(s_clipGif.name, sizeof(s_clipGif.name), mapName,
                         secs / 60u, secs % 60u);
    s_clipGif.frame  = 0;
    s_clipGif.total  = (int)frames;
    s_clipGif.failed = false;
    s_clipGif.active = true;
}

/* A clip row's export: the moment and the place the row names, for as long as
 * the round's scorer decided the clip runs. */
void lobbyClipGifStartClip(const HighlightWindow *h, const char *mapName) {
    lobbyClipGifStart(h->startMs, h->durationMs, true, h->mapX, h->mapY,
                      mapName);
}

/* The transport's export: a fixed length from wherever the playhead sits, with
 * the view left where the player put it. */
void lobbyClipGifStartFromPlayhead(uint32_t curMs, const char *mapName) {
    lobbyClipGifStart(curMs, CLIP_GIF_PLAYHEAD_MS, false, 0, 0, mapName);
}

/* The export control itself: the picture glyph where it loaded, the format's
 * name where it didn't. `compact` drops the frame padding's vertical half so
 * the button fits a one-text-line clip row; the transport's copy keeps it and
 * comes out the height of the buttons beside it. The tooltip names the format
 * either way — a glyph on its own does not say which one. (Not `small`: the
 * Windows RPC headers define that as a type.) */
bool lobbyClipGifButton(const char *id, bool compact) {
    const float lineH = ImGui::GetTextLineHeight();
    bool clicked;

    if (lobbyIcons()->picture) {
        if (compact) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
        }
        clicked = ImGui::ImageButton(id, (ImTextureID)lobbyIcons()->picture,
                                     ImVec2(lineH, lineH),
                                     ImVec2(0, 0), ImVec2(1, 1),
                                     ImVec4(0, 0, 0, 0),
                                     ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (compact) {
            ImGui::PopStyleVar();
        }
    } else {
        /* id already carries its own ## prefix, so this reads as the caption
         * with the same hidden id the icon path uses. */
        char label[48];
        snprintf(label, sizeof(label), "%s%s", CLIP_GIF_TITLE, id);
        clicked = compact ? ImGui::SmallButton(label) : ImGui::Button(label);
    }
    imguiHelpTooltip(CLIP_GIF_TITLE);
    return clicked;
}

/* Width the control above will take, for a caller placing it by hand. */
float lobbyClipGifButtonWidth(void) {
    return (lobbyIcons()->picture ? ImGui::GetTextLineHeight()
                          : ImGui::CalcTextSize(CLIP_GIF_TITLE).x)
           + ImGui::GetStyle().FramePadding.x * 2.0f;
}

/* One frame: read the fixed crop out of the viewer's render target and hand it
 * to the encoder. The lobby is mid-frame and owns the render target, so
 * whatever it was pointing at goes straight back. */
static bool lobbyClipGifCaptureFrame(void) {
    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    if (!lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) ||
        tex == NULL) {
        return false;
    }
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (renderer == NULL) {
        return false;
    }

    SDL_Texture *saved = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, (SDL_Texture *)tex)) {
        return false;
    }
    SDL_Surface *raw = SDL_RenderReadPixels(renderer, &s_clipGif.crop);
    SDL_SetRenderTarget(renderer, saved);
    if (raw == NULL) {
        return false;
    }

    /* Convert rather than assume: the readback's layout follows the render
     * target, and the encoder reads RGBA8 rows. Its own pitch goes with it —
     * a converted surface is not promised to be tightly packed. */
    SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (rgba == NULL) {
        return false;
    }
    bool ok = msf_gif_frame(&s_clipGif.enc, (uint8_t *)rgba->pixels,
                            CLIP_GIF_CS_PER_FRAME, CLIP_GIF_QUALITY,
                            rgba->pitch) != 0;
    SDL_DestroySurface(rgba);
    return ok;
}

/* Native save dialog's answer. */
typedef struct {
    char path[FILENAME_MAX];
    int  ok;
    int  done;
} ClipGifSaveState;

static void SDLCALL lobbyClipGifSaveCallback(void *userdata,
                                             const char *const *filelist,
                                             int filter) {
    ClipGifSaveState *st = (ClipGifSaveState *)userdata;
    (void)filter;
    if (filelist && filelist[0]) {
        SDL_strlcpy(st->path, filelist[0], sizeof(st->path));
        /* Not every platform's dialog applies the filter's extension to a name
         * typed without one, so a name that arrives bare gets it here — the
         * file has to open as a GIF wherever the player shares it. Matched
         * case-insensitively so a name already ending .GIF keeps the one it
         * has. If there is no room for the suffix the path stands as typed;
         * SDL_strlcat leaves it terminated either way. */
        size_t len = SDL_strlen(st->path);
        if (len > 0 &&
            (len < 4 || SDL_strcasecmp(st->path + len - 4, ".gif") != 0)) {
            SDL_strlcat(st->path, ".gif", sizeof(st->path));
        }
        st->ok = 1;
    }
    st->done = 1;
}

static bool lobbyClipGifWriteFile(const char *path, const MsfGifResult *res) {
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == NULL) {
        return false;
    }
    bool ok = (SDL_WriteIO(io, res->data, res->dataSize) == res->dataSize);
    SDL_CloseIO(io);
    return ok;
}

/* Where the bytes go, on the split windowSaveMap uses: no usable native dialog
 * under a controller, so that path names the file itself under the pref dir and
 * reports where it went; the desktop path asks. The report is the path — the
 * box's title is the format name and its icon carries the rest, so neither
 * outcome needs a sentence. */
static void lobbyClipGifSave(const MsfGifResult *res) {
    if (uiShouldUseControllerMode()) {
        char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (prefDir == NULL) {
            return;
        }
        char clipsDir[FILENAME_MAX];
        snprintf(clipsDir, sizeof(clipsDir), "%sclips", prefDir);
        SDL_CreateDirectory(clipsDir);

        char fullPath[FILENAME_MAX];
        snprintf(fullPath, sizeof(fullPath), "%s/%s.gif", clipsDir,
                 s_clipGif.name);
        SDL_free(prefDir);

        bool ok = lobbyClipGifWriteFile(fullPath, res);
        imguiMessageBoxEx(CLIP_GIF_TITLE, fullPath,
                          ok ? IMGUI_MSG_INFO : IMGUI_MSG_ERROR,
                          IMGUI_MSG_OK);
        return;
    }

    ClipGifSaveState state;
    SDL_DialogFileFilter filters[] = {
        { "GIF Images", "gif" },
    };

    memset(&state, 0, sizeof(state));

    SDL_ShowSaveFileDialog(lobbyClipGifSaveCallback, &state,
                           sdl3DrawGetWindow(), filters, 1, NULL);
    while (!state.done) {
        SDL_Event e;
        SDL_WaitEventTimeout(&e, 100);
    }
    if (state.ok && !lobbyClipGifWriteFile(state.path, res)) {
        /* The player picked the place, so silence would be the only cue that
         * nothing landed there. */
        imguiMessageBoxEx(CLIP_GIF_TITLE, state.path, IMGUI_MSG_ERROR,
                          IMGUI_MSG_OK);
    }
}

/* Drives a capture from the recap's own frames and draws the modal over it.
 * Called once per body render, after the clip rows that arm it. */
void lobbyClipGifRender(float s) {
    if (!s_clipGif.active) {
        return;
    }
    /* Opened from here rather than from the row that started the capture: a
     * popup's id is seeded from the window submitting it, and the recap body
     * is drawn from two different containers. Re-asserting it every frame the
     * capture is live is what keeps the modal with the capture if the lobby
     * swaps layouts underneath it. */
    if (!ImGui::IsPopupOpen(CLIP_GIF_POPUP)) {
        ImGui::OpenPopup(CLIP_GIF_POPUP);
    }

    MsfGifResult finished = {};
    bool         haveFinished = false;

    if (ImGui::BeginPopupModal(CLIP_GIF_POPUP, NULL,
                               ImGuiWindowFlags_AlwaysAutoResize
                               | ImGuiWindowFlags_NoCollapse
                               | ImGuiWindowFlags_NoSavedSettings)) {
        for (int i = 0; i < CLIP_GIF_FRAMES_PER_PASS &&
                        s_clipGif.frame < s_clipGif.total; i++) {
            lvEmbedStepTicks(CLIP_GIF_TICKS_PER_FRAME);
            if (!lobbyClipGifCaptureFrame()) {
                /* A refused readback or a spent encoder stops here rather than
                 * writing a clip that cuts off mid-moment. */
                s_clipGif.failed = true;
                break;
            }
            s_clipGif.frame++;
        }

        /* Both widgets take the same explicit width rather than -1: the window
         * auto-resizes, and a fill-the-rest width inside one chases its own
         * previous frame until the modal is as narrow as the button. */
        const float rowW = 280.0f * s;
        float done = (s_clipGif.total > 0)
                         ? (float)s_clipGif.frame / (float)s_clipGif.total
                         : 0.0f;
        ImGui::ProgressBar(done, ImVec2(rowW, 0.0f));

        WBUI::PushCancelStyle();
        bool cancel = ImGui::Button(langGetText(STR_CANCEL),
                                    ImVec2(rowW, 0.0f));
        WBUI::PopCancelStyle();
        if (WBUI::CancelKeyPressed()) {
            cancel = true;
        }

        if (cancel || s_clipGif.failed) {
            lobbyClipGifFinish(NULL);
            ImGui::CloseCurrentPopup();
        } else if (s_clipGif.frame >= s_clipGif.total) {
            lobbyClipGifFinish(&finished);
            haveFinished = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* Saving runs its own dialog loop, so it waits until the popup is off the
     * stack. The bytes are ours from msf_gif_end on either way. */
    if (haveFinished) {
        if (finished.data != NULL) {
            lobbyClipGifSave(&finished);
        }
        msf_gif_free(finished);
    }
}
#endif /* BOLO_RECAP_CLIP_GIF */

#if BOLO_RECAP_WBN_RATING
/* ── WinBolo.net rating & comments ────────────────────────────────
 * The finished round's own page on WinBolo.net: the aggregate stars it
 * has been given, the comments left on it, and a form to add one. All of
 * it hangs off the summary's log key, which only a round a
 * WinBolo.net-registered host uploaded ever carries — a LAN or
 * single-player round has nothing to fetch and draws nothing.
 *
 * The async lifecycle is the log viewer's comments window
 * (src/logviewer/imgui/imgui_comments.cpp): one fetch per key, polled
 * from the render, and a post that re-arms the fetch when it lands. */

/* The round's log reaches WinBolo.net a few seconds after the recap opens,
 * so the first GET can legitimately miss a round that is about to exist.
 * Bounded retries cover the upload lag without becoming a per-frame
 * request loop against a key the server will never have. */
static const int    RECAP_RATING_RETRY_MAX = 6;
static const Uint64 RECAP_RATING_RETRY_MS  = 5000;

/* Shortest gap between two nudge-driven re-reads of the round's page. */
static const Uint64 RECAP_RATING_NUDGE_MIN_MS = 10000;

/* The rating and comments the recap is holding for one round: the key they
 * belong to, the fetch that loaded them, and the add-comment form. */
typedef struct LobbyRatingState {
    /* The key everything below belongs to; a different one means the state is
     * for the previous round and is thrown away. */
    char ratingKey[ROUND_STATS_LOGKEY_LEN] = "";

    WbnCommentsFetch       *fetch         = nullptr;
    bool                    fetchComplete = false;
    int                     fetchStatus   = 0;
    char                    fetchErr[256] = "";
    std::vector<WbnComment> comments;
    float                   rating10      = 0.0f;
    int                     numRatings    = 0;

    char postMsg[256]     = "";
    int  postStatus       = 0;
    char commentText[512] = "";
    int  commentRating    = 0;

    /* Fetches spent on this key, and the earliest tick the next one may go
     * out. */
    int    fetchAttempts  = 0;
    Uint64 fetchRetryAtMs = 0;

    /* Another player posting against this round re-reads the page, so their
     * stars and comment show without waiting for the next round. The counter is
     * only watched for movement; it is consumed on every move but acted on at
     * most once per interval, so a burst of nudges cannot queue a re-read up
     * for later. The bound sits here rather than on the server because what it
     * protects is this client's traffic to WinBolo.net, and it holds whatever
     * the server or a modified client sends. */
    uint32_t ratingSeenSeq   = 0;
    Uint64   ratingNudgeAtMs = 0;

    /* Both expands, driven from our own flags the way the highlight and award
     * expands above are, so a new round's recap starts on the closed form. */
    bool showComments   = false;
    bool showAddComment = false;
} LobbyRatingState;

static LobbyRatingState s_rating = {};

/* Held outside LobbyRatingState: a whole-struct reset would either drop the
 * pointer, leaking the handle and the worker behind it, or free it — and a
 * POST cannot be cancelled, so the free would block until the request answers
 * or times out. It is left to finish instead, and the poll clears it. */
static WbnCommentPost *s_recapPost = nullptr;

/* The fetch handle is owned here, so it has to be released before the struct
 * is overwritten. Freeing it cancels the transfer, so the wait is brief. */
void lobbyRatingReset(void) {
    if (s_rating.fetch) {
        wbn_comments_fetch_free(s_rating.fetch);
    }
    s_rating = LobbyRatingState{};
    /* An in-flight post is deliberately left running — it may still complete
     * against the old key. Until it does it blocks a new post, because the post
     * button only arms while s_recapPost is null, and when it lands its result
     * overwrites the postMsg and postStatus cleared just above; the round it
     * was posted against also does not auto-refresh. Freeing it here is not the
     * answer: a POST cannot be cancelled, so the free would block the
     * round-start path until the request answers or times out. */
}

/* Point the state at the round the lobby is holding, dropping whatever the
 * previous one loaded. Keyed off the summary rather than off the block being
 * drawn: between rounds the summary is gone, the desktop panel may be flipped
 * to the map and the controller layout may be on another tab, and none of
 * those paths reach the renderer — so a render-driven reset would leave the
 * finished round's stars and comments loaded and show them for the frames
 * before the next round's summary lands. Idempotent, so both the per-frame
 * hook and the renderer can call it. */
void lobbyRatingSyncKey(ClientSim *cs, const RoundStatsSummary *st) {
    const char *key = (st && st->wbnLogKey[0] != '\0') ? st->wbnLogKey : "";
    if (strncmp(s_rating.ratingKey, key, sizeof(s_rating.ratingKey)) == 0) return;

    lobbyRatingReset();
    /* Latched, not zeroed: the counter belongs to the sim and keeps climbing
     * across rounds, so a new round starting from zero would read the running
     * total as movement and read the page back a second time. */
    s_rating.ratingSeenSeq   = clientSimGetRatingPostedSeq(cs);
    s_rating.ratingNudgeAtMs = 0;
    if (key[0] != '\0') {
        SDL_strlcpy(s_rating.ratingKey, key, sizeof(s_rating.ratingKey));
    }
}

static void lobbyRatingKick(const char *key) {
    if (s_rating.fetch || s_rating.fetchComplete) return;
    if (s_rating.fetchAttempts >= RECAP_RATING_RETRY_MAX) return;
    if (SDL_GetTicks() < s_rating.fetchRetryAtMs) return;

    s_rating.fetch = wbn_comments_fetch_start(key);
    s_rating.fetchAttempts++;
    if (!s_rating.fetch) {
        /* HTTP isn't up yet. Space the next try like a failed one rather than
         * spending the whole budget over six consecutive frames. */
        s_rating.fetchRetryAtMs = SDL_GetTicks() + RECAP_RATING_RETRY_MS;
    }
}

static void lobbyRatingPoll(ClientSim *cs) {
    if (s_rating.fetch && wbn_comments_fetch_done(s_rating.fetch)) {
        const WbnComment *raw = nullptr;
        size_t count = 0;
        int status = wbn_comments_fetch_result(s_rating.fetch, &raw, &count,
                                               s_rating.fetchErr,
                                               sizeof(s_rating.fetchErr));
        s_rating.comments.clear();
        if (raw && count > 0) {
            s_rating.comments.assign(raw, raw + count);
        }
        wbn_comments_fetch_rating(s_rating.fetch, &s_rating.rating10,
                                  &s_rating.numRatings);
        s_rating.fetchStatus = status;

        wbn_comments_fetch_free(s_rating.fetch);
        s_rating.fetch = nullptr;

        if (status == 200) {
            s_rating.fetchComplete = true;
        } else {
            /* Left incomplete so the kick above comes back for it once the
             * gap has passed, until the budget runs out. */
            s_rating.fetchRetryAtMs = SDL_GetTicks() + RECAP_RATING_RETRY_MS;
        }
    }

    if (s_recapPost && wbn_comments_post_done(s_recapPost)) {
        s_rating.postStatus =
            wbn_comments_post_result(s_recapPost, s_rating.postMsg,
                                     sizeof(s_rating.postMsg));
        wbn_comments_post_free(s_recapPost);
        s_recapPost = nullptr;

        if (s_rating.postStatus == 200 || s_rating.postStatus == 201) {
            s_rating.commentText[0] = '\0';
            s_rating.commentRating  = 0;
            /* Read the round back so the new comment and the rating it moved
             * both show. */
            s_rating.fetchComplete  = false;
            s_rating.fetchAttempts  = 0;
            s_rating.fetchRetryAtMs = 0;
            /* And tell the rest of the lobby, so their blocks read it back
             * too instead of listing this round without the new comment. */
            clientSimNetSendRatingPosted(cs, s_rating.ratingKey);
        }
    }
}

void lobbyRenderRatingBlock(ClientSim *cs, const RoundStatsSummary *st,
                                   float s) {
    lobbyRatingSyncKey(cs, st);

    if (st->wbnLogKey[0] == '\0') {
        /* Nothing on WinBolo.net to rate, so not a separator and not a
         * disabled line — the block costs the body no height at all. */
        return;
    }

    /* Someone else in the lobby has posted against this round. Take the new
     * value whether or not the fetch is allowed yet, so a burst leaves nothing
     * armed behind it, and re-arm only once the interval has passed. */
    {
        uint32_t postedSeq = clientSimGetRatingPostedSeq(cs);
        if (postedSeq != s_rating.ratingSeenSeq) {
            s_rating.ratingSeenSeq = postedSeq;
            if (SDL_GetTicks() >= s_rating.ratingNudgeAtMs) {
                s_rating.fetchComplete   = false;
                s_rating.fetchAttempts   = 0;
                s_rating.fetchRetryAtMs  = 0;
                s_rating.ratingNudgeAtMs =
                    SDL_GetTicks() + RECAP_RATING_NUDGE_MIN_MS;
            }
        }
    }

    /* Every frame: the textures are shared with the log browser, whose exit
     * destroys them, so re-entering the lobby afterwards has to be able to
     * rebuild them. Once they are up the call is an early-out. */
    imguiStarRatingLoadIcons(sdl3DrawGetRenderer());
    lobbyRatingKick(s_rating.ratingKey);
    lobbyRatingPoll(cs);

    ImGui::Separator();

    if (s_rating.fetchStatus == 200) {
        if (s_rating.numRatings > 0) {
            imguiStarRating(s_rating.rating10);
            ImGui::SameLine();
            char ratingBuf[16];
            SDL_snprintf(ratingBuf, sizeof(ratingBuf), "%.1f", s_rating.rating10);
            MessageArgs args = {};
            SDL_strlcpy(args.string1, ratingBuf, sizeof(args.string1));
            args.number = s_rating.numRatings;
            ImGui::TextUnformatted(langGetTextFmt(STR_DLGWBN_RATING, &args));
        } else {
            /* Nobody has rated the round, so an average of 0.0 out of 0 is a
             * score nobody gave it. Said the way the browser's rating column
             * says it, and with no stars, since five empty ones read as a
             * verdict rather than as an absence of one. */
            ImGui::TextDisabled("%s: --", langGetText(STR_DLGWBN_COL_RATING));
        }
    } else if (!s_rating.fetch &&
               s_rating.fetchAttempts >= RECAP_RATING_RETRY_MAX) {
        const char *err = s_rating.fetchErr[0] ? s_rating.fetchErr
                                               : langGetText(STR_DLGWBN_NETERR);
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", err);
    } else {
        ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_LOADINGDETAIL));
    }

    {
        MessageArgs args = {};
        args.number = (int)s_rating.comments.size();
        char cmtHeader[128];
        snprintf(cmtHeader, sizeof(cmtHeader), "%s###recapWbnComments",
                 langGetTextFmt(STR_DLGWBN_COMMENTS_FMT, &args));
        ImGui::SetNextItemOpen(s_rating.showComments, ImGuiCond_Always);
        s_rating.showComments = ImGui::CollapsingHeader(cmtHeader);
    }
    if (s_rating.showComments) {
        /* Height-bounded: the reel is fed whatever the body leaves unused, so
         * a list free to grow with the round's comment count would starve it. */
        ImGui::BeginChild("##recapCommentList",
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);

        /* The list is text only, so nothing in it can take focus and a round
         * with more comments than the six lines fit is out of reach on a pad.
         * The right stick pans it instead — nothing else in the lobby reads
         * that stick. Covers Steam Input and a native pad alike, +Y = down. */
        float sdx, sdy;
        if (inputGamepadGetScrollDirection(&sdx, &sdy)) {
            ImGui::SetScrollY(ImGui::GetScrollY() + sdy * ImGui::GetTextLineHeight());
        }

        if (s_rating.comments.empty()) {
            ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_NOCOMMENTS));
        } else {
            for (const WbnComment &c : s_rating.comments) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
                ImGui::TextUnformatted(c.username);
                ImGui::PopStyleColor();
                if (c.rating > 0) {
                    ImGui::SameLine();
                    imguiStarRating((float)c.rating);
                }
                if (c.time_formatted[0]) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("- %s", c.time_formatted);
                }
                ImGui::TextWrapped("  %s", c.comment);
                ImGui::Spacing();
            }
        }
        ImGui::EndChild();
    }

    /* Closed unless the player opens it, so the text field stays out of the
     * nav graph and the Deck's on-screen keyboard never comes up while the
     * recap is only being read. */
    ImGui::SetNextItemOpen(s_rating.showAddComment, ImGuiCond_Always);
    s_rating.showAddComment =
        ImGui::CollapsingHeader(langGetText(STR_DLGWBN_ADDCOMMENT));
    if (s_rating.showAddComment) {
        char wbnToken[256], wbnExpiry[256];
        gameFrontGetWinbolonetToken(wbnToken, wbnExpiry);

        if (wbnToken[0] == '\0') {
            /* Read-only, the way the section renders in game: it reports the
             * account state and, in place of a sign-in button, says where
             * accounts are changed. Signing in is a welcome-screen action —
             * the lobby only reports which account it already has. */
            imguiWinbolonetDrawSection(true);
        } else {
            float cw = ImGui::GetContentRegionAvail().x;
            /* "-" is a comment with no rating, so the combo needs to say what
             * it sets — on its own it reads as an unexplained number picker. */
            ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_COL_RATING));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80 * s);
            ImGui::Combo("##recapRating", &s_rating.commentRating,
                         "-\0 1\0 2\0 3\0 4\0 5\0 6\0 7\0 8\0 9\0 10\0");
            ImGui::SetNextItemWidth(cw);
            ImGui::InputTextWithHint("##recapCmtText",
                                     langGetText(STR_DLGWBN_HINT_COMMENT),
                                     s_rating.commentText,
                                     sizeof(s_rating.commentText));

            /* Until the fetch has found the round, WinBolo.net does not have
             * it yet and a comment posted against the key would be refused. */
            bool canPost = s_rating.commentText[0] != '\0' &&
                           s_recapPost == nullptr &&
                           s_rating.fetchStatus == 200;
            if (!canPost) ImGui::BeginDisabled();
            if (ImGui::Button(langGetText(STR_DLGWBN_POST), ImVec2(cw, 0))) {
                s_rating.postStatus = 0;
                s_rating.postMsg[0] = '\0';
                s_recapPost = wbn_comments_post_start(s_rating.ratingKey, wbnToken,
                                                      s_rating.commentText,
                                                      s_rating.commentRating);
            }
            imguiHandOnHover();
            if (!canPost) ImGui::EndDisabled();

            if (s_recapPost) {
                ImGui::TextDisabled("%s", langGetText(STR_DLGWBN_LOADINGDETAIL));
            } else if (s_rating.postStatus == 200 || s_rating.postStatus == 201) {
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%s",
                                   langGetText(STR_DLGWBN_POSTED));
            } else if (s_rating.postStatus != 0) {
                const char *err = s_rating.postMsg[0]
                                      ? s_rating.postMsg
                                      : langGetText(STR_DLGWBN_NETERR);
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", err);
            }
        }
    }
}
#endif /* BOLO_RECAP_WBN_RATING */
#endif /* !BOLO_MOBILE */

/* ----------------------------------------------------------------------
 * Per-frame lobby state
 *
 * The lobby was historically a self-contained blocking modal: every
 * piece of per-frame UI state lived in locals of imguiLobbyShow's
 * while-loop. To let the WASM client drive the same UI from its
 * non-blocking per-frame main loop (one shared ImGui context, no
 * blocking loop), that state is hoisted into this file-static struct so
 * it survives across imguiLobbyRenderFrame() calls. The blocking
 * imguiLobbyShow() seeds it once up front (and additionally loads a
 * dedicated countdown font into its private context); the WASM path
 * lazily seeds it on the first frame after entering the lobby.
 * ------------------------------------------------------------------- */
typedef struct LobbyFrameState {
    bool   active;                 /* seeded for the current lobby session? */

    /* Presentation chrome. On desktop these come from imguiLobbyShow's
     * private context setup; on WASM they are defaulted (no custom font). */
    float  s;                      /* UI scale */
    ImFont *countdownFont;         /* NULL => default font fallback */
    float  countdownFontSize;
    DialogSafeInsets safeInsets;

    /* Chat compose buffer + unread tracking. */
    char   chatInput[LOBBY_CHAT_INPUT_SIZE];
    bool   chatUnread;
    int    lastChatLen;
    bool   teamChatUnread;
    int    lastTeamChatLen;
    int    activeTab;

    /* Map-preview texture + its rebuild bookkeeping. */
    SDL_Texture *mapPreviewTex;
    bool     mapPreviewBuilt;
    uint32_t mapPreviewOwnerSig;
    uint32_t mapPreviewOwnerSeen;
    int      mapPreviewOwnerStable;
    bool     prevMapDownloadComplete;
    LobbyMapBounds mapBounds;
    char     prevMapName[128];
    bool     awaitingMapChangePacket;
    int      awaitingFrames;
    uint32_t lastMapChangeSeq;

    /* Misc per-frame trackers. */
    bool   focusReadyPending;
    int    prevCountdown;
} LobbyFrameState;

static LobbyFrameState s_lf = {};

/* The one field of the frame state anything outside core touches: chat's
 * timestamp helper appends into it. LOBBY_CHAT_INPUT_SIZE bytes. */
char *lobbyFrameChatInput(void) {
    return s_lf.chatInput;
}

/* Seed the data + default-chrome fields for a fresh lobby session.
 * imguiLobbyShow() overrides the chrome (scale / countdown font / insets)
 * afterwards with values from its private context; the WASM path keeps
 * the defaults computed here. */
static void lobbyFrameInitState(ClientSim *cs) {
    SDL_Window *window = sdl3DrawGetWindow();

    int screenW = 1024, screenH = 768;
    if (window) {
        SDL_GetWindowSize(window, &screenW, &screenH);
        if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    }
    float s = dialogComputeScale(screenW, screenH);
#if !BOLO_MOBILE
    if (!uiModeIsSteamDeck()) s = 1.0f;
#endif
    s_lf.s                 = s;
    s_lf.countdownFont     = NULL;   /* default font; desktop overrides */
    s_lf.countdownFontSize = (s <= 1.05f) ? 54.0f : 60.0f * s;
    s_lf.safeInsets        = dialogGetSafeInsets(window);

    s_lf.chatInput[0]   = '\0';
    s_lf.chatUnread     = false;
    s_lf.lastChatLen    = 0;
    s_lf.teamChatUnread = false;
    s_lf.lastTeamChatLen = 0;
    s_lf.activeTab      = 0;

    s_lf.mapPreviewTex         = NULL;
    s_lf.mapPreviewBuilt       = false;
    s_lf.mapPreviewOwnerSig    = 0xFFFFFFFFu;
    s_lf.mapPreviewOwnerSeen   = 0xFFFFFFFFu;
    s_lf.mapPreviewOwnerStable = 0;
    s_lf.prevMapDownloadComplete = clientSimIsMapDownloadComplete(cs);
    s_lf.mapBounds.minX = 0;
    s_lf.mapBounds.minY = 0;
    s_lf.mapBounds.maxX = MAP_PREVIEW_SIZE - 1;
    s_lf.mapBounds.maxY = MAP_PREVIEW_SIZE - 1;
    s_lf.prevMapName[0] = '\0';
    {
        const char *curName = clientSimGetMapName(cs);
        if (curName) SDL_strlcpy(s_lf.prevMapName, curName, sizeof(s_lf.prevMapName));
    }
    s_lf.awaitingMapChangePacket = false;
    s_lf.awaitingFrames          = 0;
    s_lf.lastMapChangeSeq        = clientSimGetLobbyMapChangeSeq(cs);

    s_lf.focusReadyPending = uiShouldUseControllerMode();
    s_lf.prevCountdown     = clientSimGetCountdownSeconds(cs);
}

/* Release per-frame lobby state (map-preview texture, popup buffers, and
 * every transient visibility/pending flag). The blocking imguiLobbyShow()
 * calls this on teardown; the WASM host calls it when leaving the lobby.
 * Resets every file-scope flag that could render UI on the next entry if a
 * disconnect (or any other exit) caught the dialog mid-action. The
 * MapChooserState caches stay populated (next open re-uses the discovered
 * map list / preview view); only the visibility / focus / pending-action
 * flags reset. */
extern "C" void imguiLobbyFrameReset(void) {
    if (s_lf.mapPreviewTex) {
        SDL_DestroyTexture(s_lf.mapPreviewTex);
        s_lf.mapPreviewTex = NULL;
    }
    lobbyMapPreviewReset();
    mapPreviewPopupDestroy();

    lobbyChooserReset();

    lobbyChatReset();

    /* A lobby re-entered with a summary still stored should open on the
     * recap, not on whatever the last session was left looking at. */
    lobbyRecapReset();

#if !BOLO_MOBILE
    /* The reel holds the viewer's decoder singleton — never leave it running
     * past the lobby session. */
    lobbyReelEnd();
#endif
#if !BOLO_MOBILE && BOLO_RECAP_WBN_RATING
    /* Release the rating fetch and everything it filled in. The star textures
     * stay: they belong to the WBN browser as much as to the recap, and the
     * loader rebuilds them on demand. */
    lobbyRatingReset();
#endif

#ifndef __EMSCRIPTEN__
    /* A single-player map fetch left in flight holds a whole map's bytes in
     * its result slot; nothing drains it once the lobby is gone. */
    lobbySpWbnReset();
#endif

    lobbyPlayersReset();

    lobbyCommandReset();

    s_lf.active = false;
}

/* Build the lobby UI into the currently-active ImGui frame. See
 * imgui_lobby.h for the host contract. Returns LOBBY_FRAME_LEFT once the
 * player confirms leaving, otherwise LOBBY_FRAME_CONTINUE. */
extern "C" LobbyFrameStatus imguiLobbyRenderFrame(ClientSim *cs) {
    if (!s_lf.active) { lobbyFrameInitState(cs); s_lf.active = true; }

#if !BOLO_MOBILE && BOLO_RECAP_WBN_RATING
    /* Before anything draws, so a round that has ended takes its rating and
     * comments with it whether or not the recap is the view on screen. */
    lobbyRatingSyncKey(cs, cs ? clientSimGetLastRoundStats(cs) : NULL);
#endif

    SDL_Window   *window   = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return LOBBY_FRAME_CONTINUE;

    /* A tankless spectator views the live lobby read-only: it owns no
     * player slot, so every "is this me / am I host" branch below is
     * forced off and every mutating control is hidden or disabled. */
    const bool spectator = clientSimIsSpectator(cs);

    /* Presentation chrome, seeded by the host (desktop) or
     * lobbyFrameInitState (WASM). */
    const float s = s_lf.s;
    ImFont *countdownFont = s_lf.countdownFont;
    const float countdownFontSize = s_lf.countdownFontSize;
    DialogSafeInsets &safeInsets = s_lf.safeInsets;

    /* Team combo items */
    const char *teamItems[] = {
        langGetText(STR_NONE), "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16"
    };
    const int kAwaitingMaxFrames = 60;

    /* Aliases onto the persistent per-frame state so the UI body below
     * reads/writes it by its original local names. */
    char (&chatInput)[LOBBY_CHAT_INPUT_SIZE]    = s_lf.chatInput;
    bool &chatUnread                      = s_lf.chatUnread;
    int  &lastChatLen                     = s_lf.lastChatLen;
    bool &teamChatUnread                  = s_lf.teamChatUnread;
    int  &lastTeamChatLen                 = s_lf.lastTeamChatLen;
    int  &activeTab                       = s_lf.activeTab;
    SDL_Texture *&mapPreviewTex           = s_lf.mapPreviewTex;
    bool &mapPreviewBuilt                 = s_lf.mapPreviewBuilt;
    uint32_t &mapPreviewOwnerSig          = s_lf.mapPreviewOwnerSig;
    uint32_t &mapPreviewOwnerSeen         = s_lf.mapPreviewOwnerSeen;
    int  &mapPreviewOwnerStable           = s_lf.mapPreviewOwnerStable;
    bool &prevMapDownloadComplete         = s_lf.prevMapDownloadComplete;
    LobbyMapBounds &mapBounds                  = s_lf.mapBounds;
    char (&prevMapName)[128]              = s_lf.prevMapName;
    bool &awaitingMapChangePacket         = s_lf.awaitingMapChangePacket;
    int  &awaitingFrames                  = s_lf.awaitingFrames;
    uint32_t &lastMapChangeSeq            = s_lf.lastMapChangeSeq;
    bool &focusReadyPending               = s_lf.focusReadyPending;
    int  &prevCountdown                   = s_lf.prevCountdown;

    /* Set when the player confirms the Leave dialog; the host disconnects. */
    bool leftLobby = false;

        gameFrontTickSteamPresenceLobby(cs);

        bool hasTransport = clientSimHasTransport(cs);

        /* Clear balance proposal when countdown starts */
        if (clientSimGetCountdownSeconds(cs) > 0 && clientSimIsBalanceProposalActive(cs)) {
            clientSimSetBalanceProposalActive(cs, false);
            clientSimClearBalanceProposal(cs);
        }

        /* Countdown tick: one cue per second while the start countdown runs.
         * clientSimGetCountdownSeconds decrements once per second, so playing
         * only on a change to a positive value yields one tick per second
         * (5,4,3,2,1); reaching 0 is the game start, handled below. */
        int curCountdown = clientSimGetCountdownSeconds(cs);
        if (clientSimGetNetStatus(cs) == netLobbyCountdown &&
            curCountdown > 0 && curCountdown != prevCountdown &&
            !clientSimIsSinglePlayer(cs)) {
            /* SP starts instantly: the in-process server runs the real
             * countdown state but isn't rate-limited to 50 Hz, so it burns
             * all 250 ticks in ~a frame. The client still receives the
             * initial CTRL_GAME_PHASE_COUNTDOWN (secs=5) and would play one
             * stray leading tick before RUNNING arrives. SP has no real-time
             * countdown to sonify; gate it out. MP keeps 5,4,3,2,1. */
            soundPlayEffect(lobbyCountdown);
        }
        prevCountdown = curCountdown;


        /* Reset preview on either signal:
         *   1. Map download invalidated (server-driven re-download
         *      cycle — MP path triggers this via NotifyMapChange).
         *   2. Map name changed (SP-host Set Map doesn't run the
         *      download cycle, but the name DOES change; detect that
         *      so we can drop the stale texture instead of slapping
         *      a fresh render on top of it). */
        bool downloadInvalidated =
            !clientSimIsMapDownloadComplete(cs) && prevMapDownloadComplete;
        const char *curMapName = clientSimGetMapName(cs);
        bool nameChanged = (curMapName != NULL) &&
            SDL_strcmp(prevMapName, curMapName) != 0;
        /* Sequence-number edge — bumps when the client receives a
         * MAP_CHANGE packet. Catches the MP-host loopback case
         * where the !complete-then-complete transition lives inside
         * a single frame and the boolean edge detector misses it. */
        uint32_t curMapChangeSeq = clientSimGetLobbyMapChangeSeq(cs);
        bool seqChanged = (curMapChangeSeq != lastMapChangeSeq);
        lastMapChangeSeq = curMapChangeSeq;

        /* Tear down the stale texture on ANY signal that the map
         * identity changed. Without this the user sees the OLD
         * preview rendered behind the new one when nameChanged
         * arrives via the in-process subscriber path before the
         * MAP_CHANGE packet arrives over UDP. The gap between the
         * texture clear and the rebuild is covered by
         * awaitingMapChangePacket (when only nameChanged fired) or
         * by the standard !complete "Downloading…" gate (once
         * seqChanged / downloadInvalidated fires). */
        if (downloadInvalidated || seqChanged || nameChanged) {
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[LOBBY/PREVIEW] reset: downloadInvalidated=%d seqChanged=%d nameChanged=%d curName='%s' prevName='%s' seq=%u complete=%d hasTransport=%d isSP=%d",
                (int)downloadInvalidated, (int)seqChanged, (int)nameChanged,
                curMapName ? curMapName : "(null)",
                prevMapName,
                (unsigned)curMapChangeSeq,
                (int)clientSimIsMapDownloadComplete(cs),
                (int)hasTransport,
                cs ? (int)clientSimIsSinglePlayer(cs) : -1);
            /* Intentionally do NOT destroy mapPreviewTex here — keeping
             * the OLD preview visible until the rebuild has new bytes
             * ready avoids a visible flash to "Downloading…" between
             * the map-change signal and the chunk-redownload finishing.
             * The rebuild block below atomically destroys-and-replaces
             * the texture once fresh data is in hand.
             *
             * Popup compressed data IS dropped so the big-preview view
             * doesn't render against stale bytes if the user opens it
             * during the gap. The popup window itself is NOT closed —
             * if the user has it open we want it to seamlessly update
             * to the new map (handled in the rebuild block below via
             * mapPreviewPopupRefreshOpen). */
            if (lobbyMapPreview()->popupCompressedData) { SDL_free(lobbyMapPreview()->popupCompressedData); lobbyMapPreview()->popupCompressedData = NULL; lobbyMapPreview()->popupCompressedLen = 0; }
        }
        /* mapPreviewBuilt — open the rebuild gate ONLY when we have
         * a real data signal (seq tick OR download-complete edge).
         * Resetting on nameChanged alone would fire the rebuild path
         * one frame later against the still-stale mapDownloadBuf and
         * silently re-render the old map. */
        if (downloadInvalidated || seqChanged) {
            mapPreviewBuilt = false;
            awaitingMapChangePacket = false;
            awaitingFrames = 0;
        }
        /* Safety timeout: if we've been waiting for MAP_CHANGE for
         * too long (e.g. server forgot to push it for some reason),
         * force a rebuild from whatever bytes the download buffer
         * currently holds. Better to show a possibly-stale preview
         * than to leave the panel stuck on "Downloading…". */
        if (awaitingMapChangePacket) {
            awaitingFrames++;
            if (awaitingFrames > kAwaitingMaxFrames) {
                WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[LOBBY/PREVIEW] awaiting MAP_CHANGE timed out after %d frames; forcing rebuild",
                    awaitingFrames);
                awaitingMapChangePacket = false;
                awaitingFrames = 0;
                mapPreviewBuilt = false;
            }
        }
        /* nameChanged on a UDP transport raises the wait flag — we
         * know fresh bytes are coming over the wire but haven't
         * received the seq tick yet. Local-transport clients (SP
         * host AND the host's own client in a Local MP game) never
         * see a MAP_CHANGE packet because there's no UDP socket
         * round-trip; leaving the flag set would lock the preview
         * on "Downloading…" forever. Gate on isUdpTransport so the
         * flag only flips when we're genuinely waiting on the wire.
         * Local-transport clients open the rebuild gate immediately
         * since the sim is in-process and the new bytes are
         * available right now. */
        if (nameChanged) {
            bool isUdp = cs && clientSimIsUdpTransport(cs);
            if (hasTransport && isUdp) {
                awaitingMapChangePacket = true;
            } else {
                mapPreviewBuilt = false;
            }
            SDL_strlcpy(prevMapName, curMapName, sizeof(prevMapName));
        }
        prevMapDownloadComplete = clientSimIsMapDownloadComplete(cs);

        /* Build map preview once download completes. Two data
         * sources:
         *   - MP: clientSimGetServerMapData reads the just-downloaded
         *     bytes from the UDP transport's mapDownloadBuf.
         *   - SP: there's no UDP transport, so we pull the compressed
         *     map straight from gameFrontGetSinglePlayerServerSim(). */
        if (clientSimIsMapDownloadComplete(cs) && !mapPreviewBuilt && hasTransport) {
            int mapLen = 0;
            const BYTE *mapData = clientSimGetServerMapData(cs, &mapLen);
            const char *dataSource = (mapData && mapLen > 0) ? "udp" : "(udp returned null)";
            BYTE spBuf[65536];
            if ((!mapData || mapLen <= 0) &&
                cs && !clientSimIsUdpTransport(cs)) {
                /* Non-UDP transport (SP host OR MP-host's own client)
                 * — no UDP buffer to pull from. Read the compressed
                 * map straight from the in-process ServerSim instead. */
                ServerSim *spSim = gameFrontGetSinglePlayerServerSim();
                if (spSim) {
                    mapLen  = serverSimGetCompressedMap(spSim, spBuf);
                    mapData = (mapLen > 0) ? spBuf : NULL;
                    dataSource = "local-direct";
                }
            }
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[LOBBY/PREVIEW] rebuild attempt: source=%s mapLen=%d mapData=%p mapPreviewBuilt(prior)=0 hasTransport=%d",
                dataSource, mapLen, (const void *)mapData, (int)hasTransport);
            if (mapData && mapLen > 0) {
                /* Build the NEW texture before destroying the OLD one
                 * so the display layer (which polls mapPreviewTex) is
                 * never left looking at a NULL pointer between frames
                 * — no flicker through "Map unavailable" / "Downloading…"
                 * placeholders. */
                SDL_Texture *prev = mapPreviewTex;
                uint8_t owners0[MAX_STARTS];
                uint32_t sig0 = 0;
                int nOwn0 = lobbyComputeStartOwners(cs, spectator ? -1 : (int)gameFrontGetPlayerNum(),
                                                    owners0, MAX_STARTS, &sig0);
                mapPreviewTex = lobbyBuildMapPreview(renderer, mapData, mapLen, &mapBounds,
                                                nOwn0 ? owners0 : NULL, nOwn0);
                mapPreviewOwnerSig = sig0;
                if (prev) SDL_DestroyTexture(prev);
                WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[LOBBY/PREVIEW] rebuild done: tex=%p bounds=(%d..%d, %d..%d)",
                    (const void *)mapPreviewTex,
                    mapBounds.minX, mapBounds.maxX,
                    mapBounds.minY, mapBounds.maxY);
                /* Stash for popup decompression */
                if (lobbyMapPreview()->popupCompressedData) { SDL_free(lobbyMapPreview()->popupCompressedData); lobbyMapPreview()->popupCompressedData = NULL; }
                lobbyMapPreview()->popupCompressedData = (BYTE *)SDL_malloc(mapLen);
                if (lobbyMapPreview()->popupCompressedData) {
                    SDL_memcpy(lobbyMapPreview()->popupCompressedData, mapData, mapLen);
                    lobbyMapPreview()->popupCompressedLen = mapLen;
                    /* Recompute the per-start compass cache from the new
                     * map bytes — only here, so the player-list column
                     * never decompresses the map per frame. */
                    lobbyRebuildStartCompassCache(lobbyMapPreview()->popupCompressedData,
                                             lobbyMapPreview()->popupCompressedLen);
                    /* If the user has the big map-preview popup open
                     * right now, refresh its underlying data in place
                     * so it seamlessly updates to the new map instead
                     * of closing on every server-side map change. */
                    if (mapPreviewPopupIsOpen()) {
                        mapPreviewPopupRefreshOpen(lobbyMapPreview()->popupCompressedData,
                                                    lobbyMapPreview()->popupCompressedLen);
                    }
                }
            }
            mapPreviewBuilt = true;
        }

        /* Recolour the preview in place when start ownership / teams change.
         * Claims don't trigger a map re-download, so the build-once path
         * above won't catch them. Cheap: rebuilds the 256² minimap only when
         * the ownership signature actually moves. */
        if (mapPreviewTex && lobbyMapPreview()->popupCompressedData && lobbyMapPreview()->popupCompressedLen > 0) {
            uint8_t owners[MAX_STARTS];
            uint32_t sig = 0;
            int nOwn = lobbyComputeStartOwners(cs, spectator ? -1 : (int)gameFrontGetPlayerNum(),
                                               owners, MAX_STARTS, &sig);
            /* Debounce: wait until ownership has held steady for a few frames
             * before the (heavy) texture rebuild, so a burst of changes can't
             * rebuild every frame and stall the UI. */
            if (sig == mapPreviewOwnerSeen) {
                if (mapPreviewOwnerStable < 1000) mapPreviewOwnerStable++;
            } else {
                mapPreviewOwnerSeen   = sig;
                mapPreviewOwnerStable = 0;
            }
            if (sig != mapPreviewOwnerSig && mapPreviewOwnerStable >= 3) {
                SDL_Texture *fresh = lobbyBuildMapPreview(renderer, lobbyMapPreview()->popupCompressedData,
                                                     lobbyMapPreview()->popupCompressedLen, &mapBounds,
                                                     nOwn ? owners : NULL, nOwn);
                if (fresh) {
                    SDL_DestroyTexture(mapPreviewTex);
                    mapPreviewTex = fresh;
                }
                mapPreviewOwnerSig = sig;
            }
        }

        /* Query window size */
        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        /* Render popup tiles to offscreen texture before ImGui frame */
        mapPreviewPopupRenderOffscreen(renderer, winW, winH);


        /* Full-screen host window with safe area padding */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        float padL = ImGui::GetStyle().WindowPadding.x + safeInsets.left;
        float padR = safeInsets.right;
        float padT = ImGui::GetStyle().WindowPadding.y + safeInsets.top;
        float padB = safeInsets.bottom;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padL, padT));
        ImGui::Begin("##LobbyBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        bool wantLeaveConfirm = false;

        BYTE myPlayerNum = gameFrontGetPlayerNum();

        /* --- Header: Server info line --- */
        {
            /* Server address (loopback->LAN and host external-NAT
             * substitution) plus async reverse-DNS and the clickable
             * join-link are all handled by the shared GUI helper. */
            char dispIp[64];
            unsigned dispPort = 0;
            bool haveServerAddr =
                guiServerDisplayAddress(cs, dispIp, sizeof(dispIp), &dispPort);

            /* Hide the host's server IP from joined clients on Internet
             * games so lobby screenshots don't leak the address. Host
             * still sees the real address so they can read it to
             * friends. LAN-only joiners keep the IP (it's already on a
             * local network). SP always shows "Single player".
             *
             * Module-static flag rather than a compile-time #define so
             * it can be flipped at runtime later (settings toggle, INI
             * pref, console command) without rebuilding callers. */
            static bool s_hideServerIpFromJoiners = false;
            bool serverIsPrivate =
                s_hideServerIpFromJoiners &&
                (myPlayerNum != 0 || spectator) &&
                !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs);
            bool showServerLink = haveServerAddr && !serverIsPrivate;

            /* Renders the server value: a clickable join-link when we have a
             * real address, otherwise the SP / hidden-Internet placeholder. */
            auto renderServerValue = [&]() {
                if (showServerLink) {
                    guiServerAddressLink(cs, dispIp, dispPort);
                } else {
                    ImGui::TextUnformatted(
                        clientSimIsSinglePlayer(cs)
                            ? langGetText(STR_DLGLOBBY_SERVERDISP_SP)
                            : langGetText(STR_DLGLOBBY_SERVERDISP_INTERNET));
                }
            };

            char timeStr[32];
            lobbyFormatTimeLimit(clientSimGetLobbyTimeLimit(cs), timeStr, sizeof(timeStr));

#if BOLO_MOBILE
            /* Stack labels vertically on mobile so the line wraps cleanly. */
            ImGui::TextUnformatted(langGetText(STR_DLGNETINFO_SERVER));
            ImGui::SameLine();
            renderServerValue();
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), lobbyGameTypeStr(clientSimGetLobbyGameType(cs)));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        clientSimIsLobbyHiddenMines(cs) ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), lobbyAiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#else
            /* Leave button sits at the top-left, before the Server: line.
             * Escape key also opens the leave confirmation popup. Rendered as
             * a back arrow (left-pointing triangle) drawn into a normal-height
             * button so it matches Add Team / Ready visually without depending
             * on geometric-shape glyphs being present in the active font. */
            bool leaveClicked = false;
            if (!uiShouldUseControllerMode()) {
                float leaveBtnH = ImGui::GetFrameHeight();
                float leaveBtnW = leaveBtnH * 1.4f;
                ImVec2 leaveBtnPos = ImGui::GetCursorScreenPos();
                leaveClicked = ImGui::Button("##leave", ImVec2(leaveBtnW, leaveBtnH));
                {
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    float cx = leaveBtnPos.x + leaveBtnW * 0.5f;
                    float cy = leaveBtnPos.y + leaveBtnH * 0.5f;
                    float r  = leaveBtnH * 0.28f;
                    ImVec2 p1(cx - r,         cy);
                    ImVec2 p2(cx + r * 0.7f,  cy - r);
                    ImVec2 p3(cx + r * 0.7f,  cy + r);
                    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
                    dl->AddTriangleFilled(p1, p2, p3, col);
                }
            }
            if (leaveClicked ||
                (((ImGui::IsKeyPressed(ImGuiKey_Escape) && !mapPreviewPopupIsOpen() && !s_chooser.open && (uiShouldUseControllerMode() ? (!ImGui::GetIO().WantTextInput && !keyboardIsOpen()) : !dialogNavWasInsideSubRegionAtFrameStart())) ||
                  (ImGui::IsKeyPressed(ImGuiKey_W) && IMGUI_PRIMARY_KEY_DOWN())
#ifdef __APPLE__
                  || (ImGui::IsKeyPressed(ImGuiKey_Period) && ImGui::GetIO().KeySuper)
#endif
                 ) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                wantLeaveConfirm = true;
            }
            if (!uiShouldUseControllerMode()) {
                ImGui::SameLine(0, 16);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(langGetText(STR_DLGNETINFO_SERVER));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            renderServerValue();
            ImGui::SameLine(0, 16);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), lobbyGameTypeStr(clientSimGetLobbyGameType(cs)));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        clientSimIsLobbyHiddenMines(cs) ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), lobbyAiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#endif

            /* Layout A — connectivity badge in the top-right corner.
             * Only meaningful when this client is also hosting AND
             * the host instance is actively NAT-punching (i.e. not
             * passed -no-natpunch / LAN-only). Skipping it covers SP,
             * LAN-only hosts, and non-hosting clients in one check. */
            if (serverInstanceIsNatPunchActive()) {
                /* lobbyRenderConnectivityBadge right-aligns itself within the
                 * remaining horizontal space, so we just SameLine onto
                 * the status row and let it absorb the slack. */
                ImGui::SameLine();
                lobbyRenderConnectivityBadge(renderer, s);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Layout A — surface the most recent server reject (locked
         * setting, non-host action, invalid request). Renders only
         * when clientSimGetLobbyLastRejectPacket(cs) != 0. */
        lobbyRenderRejectToast(cs, s);

        /* Layout A — collapsible game settings panel (radios, checkboxes,
         * lock badges). Edits dispatch via PACKET_LOBBY_SET_SETTING.
         * Skipped entirely for non-privileged players — the same
         * info already lives in the top status bar, and the panel
         * is read-only anyway. */
        bool gsEffectiveHost = !spectator && (lobbyIsHost(cs, myPlayerNum)
            || clientSimGetLobbyOpenHost(cs)
            || (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                 & PLAYER_FLAG_ADMIN)));
        /* Tabbed (controller) vs two-column (mouse) layout. Computed here so
         * the shared settings panel below renders only on the mouse path —
         * the tabbed layout shows settings in its own Settings tab instead. */
#if BOLO_MOBILE
        const bool useTabbedLobby = true;
#else
        const bool useTabbedLobby = uiShouldUseControllerMode();
#endif
#if POSTGAME_STATS_ENABLED
        const bool lobbyShowLastRound = clientSimGetLastRoundStats(cs) != NULL;
#else
        const bool lobbyShowLastRound = false;  /* post-game recap withheld this release */
#endif
        /* The countdown clearing the summary also clears the map view and the
         * clip expand, so the next round's recap opens on itself rather than on
         * wherever the player left the panel. */
        if (!lobbyShowLastRound) {
            lobbyRecapReset();
        }
        /* Fold the settings header away for the post-game view and put it
         * back when the countdown clears the summary. Called every frame,
         * acts only on the transitions. */
        lobbySettingsPostGameEdge(lobbyShowLastRound);

        /* Settings above the layout is the two-column (mouse) path only; the
         * tabbed layout renders the same form in a dedicated tab, so skip it
         * here to avoid double-rendering it for the host. */
        if (gsEffectiveHost && !useTabbedLobby) {
            lobbyRenderGameSettingsPanel(cs, myPlayerNum, s);
            ImGui::Separator();
            ImGui::Spacing();
        }

        /* Hosted-MP port-mapping status now renders at top-right via
         * lobbyRenderConnectivityBadge — see the call site in the read-only
         * status block above. */

        /* --- Main content (tabbed in controller mode, two-column for mouse;
         * useTabbedLobby computed above the shared settings panel) --- */
        if (useTabbedLobby) {
            float availW = ImGui::GetContentRegionAvail().x - padR;
            float btnAreaH = ImGui::GetTextLineHeightWithSpacing() * 2 + 16.0f * s;

            /* Detect new chat messages for unread indicator */
            int chatLen = (int)SDL_strlen(clientSimGetLobbyChatHistory(cs));
            if (chatLen > lastChatLen && activeTab != 3) {
                chatUnread = true;
            }
            lastChatLen = chatLen;
            int teamChatLen = (int)SDL_strlen(clientSimGetLobbyTeamChatHistory(cs));
            if (teamChatLen > lastTeamChatLen && activeTab != 4) {
                teamChatUnread = true;
            }
            lastTeamChatLen = teamChatLen;

            /* Trigger-driven tab cycling. Settings (host-only) and Team-chat
             * (on-team-only) are conditional, so step over an explicit list of
             * the tabs actually drawn this frame — a plain modulo could land on
             * a missing index. A native pad feeds L1/R1 even with ImGui gamepad
             * nav off; under Steam Input the pad is hidden from SDL, so the
             * menu_tab_left/right actions (mapped to LT/RT) arrive via
             * imguiSteamNavConsumeMenuTabShift instead. Suppressed while the
             * Choose Map window is open so the trigger press cycles its source
             * tabs (rendered later this frame) instead of the lobby tabs. */
            /* The recap tab exists only while a stored end-of-round
             * summary does (set at game over, cleared on countdown). */
            const bool haveLastRound = lobbyShowLastRound;
            if (!s_chooser.open) {
                const ClientLobbySlot *myTabSlot =
                    clientSimGetLobbySlot(cs, myPlayerNum);
                bool onTeam = !spectator && myTabSlot && myTabSlot->teamNumber != 0;
                int shift = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0)
                          - (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? 1 : 0);
                if (shift == 0)
                    shift = imguiSteamNavConsumeMenuTabShift();
                if (shift != 0) {
                    /* Visible tab indices in render order. The Settings
                     * predicate must match the Settings tab's BeginTabItem
                     * gate (gsEffectiveHost) exactly. */
                    int vis[6];
                    int nVis = 0;
                    vis[nVis++] = 0;                       /* Players */
                    vis[nVis++] = 1;                       /* Map */
                    if (gsEffectiveHost) vis[nVis++] = 2;  /* Settings */
                    vis[nVis++] = 3;                       /* Chat */
                    if (onTeam) vis[nVis++] = 4;           /* Team */
                    if (haveLastRound) vis[nVis++] = 5;    /* Last round */
                    int cur = 0;
                    for (int i = 0; i < nVis; i++) {
                        if (vis[i] == activeTab) { cur = i; break; }
                    }
                    *lobbyPlayersForceTab() = vis[(cur + shift + nVis) % nVis];
                }
            }

            if (ImGui::BeginTabBar("##LobbyTabs")) {
                /* --- Players tab --- */
                if (ImGui::BeginTabItem(langGetText(STR_MENU_PLAYERS), nullptr,
                        *lobbyPlayersForceTab() == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    activeTab = 0;
                    /* Allow New Players row above the player list (mobile). */
                    lobbyRenderAllowNewPlayersRow(cs, myPlayerNum, s);
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                    ImGui::BeginChild("##PlayerPanel", ImVec2(availW, tabH), ImGuiChildFlags_NavFlattened);

                    /* Layout A: team-grouped player rendering. The
                     * legacy 5-column table below the #if 0 is left
                     * intact for reference; toggle the 0/1 to A/B
                     * compare during the in-progress UI rewrite. */
#if 1
                    bool isHostHere = lobbyIsHost(cs, myPlayerNum);
                    lobbyRenderTeamGroupedPlayers(cs, myPlayerNum, s, isHostHere);
                    /* Avoid the legacy table entirely. */
                    if (false) {
#else
                    if (ImGui::BeginTable("##PlayerTable", 5,
                                          ImGuiTableFlags_Borders |
                                          ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_ScrollY)) {
#endif
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_PLAYER_COL), ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_PING_COL), ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_TEAM_COL), ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                        ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_READY_COL), ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 65.0f * s);
                        ImGui::TableHeadersRow();

                        bool botsAllowed = (clientSimGetLobbyAiType(cs) != 0);

                        WB_LOG_TRACE(WB_LOG_CAT_GUI, "[LOBBY DBG] rendering player table");
                        for (int i = 0; i < MAX_TANKS; i++) {
                            ImGui::TableNextRow();

                            const ClientLobbySlot *slot = clientSimGetLobbySlot(cs, (BYTE)i);
                            if (slot && slot->connected) {
                                bool isMe = (i == myPlayerNum);

                                /* Player Name (with flag) */
                                ImGui::TableSetColumnIndex(0);
                                if (slot->countryCode[0] != '\0') {
                                    if (drawCountryFlagWithTip(slot->countryCode)) {
                                        ImGui::SameLine();
                                    }
                                }
                                if (!slot->isBot) {
                                    uint8_t pflags = slot->clientFlags;
                                    if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                                        pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
                                    }
                                    renderPlayerName(NULL,
                                                     pflags,
                                                     slot->clientType,
                                                     "", false);
                                }
                                if (slot->isBot) {
                                    MessageArgs args = {};
                                    strncpy(args.playerName, slot->playerName, sizeof(args.playerName) - 1);
                                    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                                                       langGetTextFmt(STR_DLGLOBBY_BOT_FMT, &args));
                                } else if (isMe) {
                                    MessageArgs args = {};
                                    strncpy(args.playerName, slot->playerName, sizeof(args.playerName) - 1);
                                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s",
                                                       langGetTextFmt(STR_DLGLOBBY_YOU_FMT, &args));
                                } else {
                                    ImGui::Text("%s", slot->playerName);
                                }

                                /* Ping */
                                ImGui::TableSetColumnIndex(1);
                                if (slot->pingMs > 0) {
                                    ImVec4 pingColor =
                                        imguiPingBandColor(pingBandClassify(slot->pingMs));
                                    ImGui::TextColored(pingColor, "%dms", (int)slot->pingMs);
                                } else {
                                    ImGui::TextDisabled("-");
                                }

                                /* Team */
                                ImGui::TableSetColumnIndex(2);
                                if (isMe && hasTransport) {
                                    int teamIdx = slot->teamNumber;
                                    ImGui::SetNextItemWidth(-1);
                                    char comboId[16];
                                    SDL_snprintf(comboId, sizeof(comboId), "##team%d", i);
                                    if (ImGui::Combo(comboId, &teamIdx, teamItems, 17)) {
                                        lobbySendTeamSet(cs, (uint8_t)myPlayerNum, (uint8_t)teamIdx);
                                    }
                                } else {
                                    if (slot->teamNumber > 0) {
                                        ImGui::Text("%d", slot->teamNumber);
                                    } else {
                                        ImGui::TextDisabled("%s", langGetText(STR_NONE));
                                    }
                                }
                                if (clientSimIsBalanceProposalActive(cs) && clientSimGetBalanceProposal(cs, (BYTE)i) != 0 &&
                                    clientSimGetBalanceProposal(cs, (BYTE)i) != slot->teamNumber) {
                                    ImGui::SameLine();
                                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "-> %d", clientSimGetBalanceProposal(cs, (BYTE)i));
                                }

                                /* Ready */
                                ImGui::TableSetColumnIndex(3);
                                if (slot->ready) {
                                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", langGetText(STR_YES));
                                } else {
                                    ImGui::TextDisabled("%s", langGetText(STR_NO));
                                }

                                /* Action */
                                ImGui::TableSetColumnIndex(4);
                                if (slot->isBot && hasTransport) {
                                    char btnId[64];
                                    SDL_snprintf(btnId, sizeof(btnId), "%s##%d", langGetText(STR_DLGLOBBY_REMOVE), i);
                                    if (ImGui::SmallButton(btnId)) {
                                        lobbySendRemoveBot(cs, (uint8_t)i);
                                    }
                                }
                            } else {
                                /* Empty slot */
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("---");
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(2);
                                ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(3);
                                ImGui::TextDisabled("-");
                                ImGui::TableSetColumnIndex(4);
                                if (botsAllowed && hasTransport) {
                                    char btnId[64];
                                    SDL_snprintf(btnId, sizeof(btnId), "%s##%d", langGetText(STR_DLGLOBBY_ADDBOT), i);
                                    bool pending = lobbyAddBotPending(cs);
                                    if (pending) ImGui::BeginDisabled();
                                    if (ImGui::SmallButton(btnId)) {
                                        lobbySendAddBotDebounced(cs, -1, 0);
                                    }
                                    if (pending) ImGui::EndDisabled();
                                }
                            }
                        }
                        ImGui::EndTable();
                    }

                    ImGui::EndChild(); /* ##PlayerPanel */
                    ImGui::EndTabItem();
                }

                /* --- Map tab --- */
                if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_MAP_TAB), nullptr,
                        *lobbyPlayersForceTab() == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    activeTab = 1;
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;

                    /* "Choose Map" — opens the separate chooser window.
                     * Host / admin / openHost-allowed only; non-privileged
                     * clients never see the button. Server enforces the
                     * same authority gate on PACKET_LOBBY_SET_MAP.
                     * effHostMap stays visible to the preview block below
                     * so privileged users can also click the preview to
                     * jump straight into the chooser. */
                    bool isHostLocal  = lobbyIsHost(cs, myPlayerNum);
                    bool isAdminLocal = (myPlayerNum < MAX_TANKS &&
                        (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                         & PLAYER_FLAG_ADMIN));
                    bool effHostMap = !spectator && (isHostLocal || isAdminLocal ||
                                      clientSimGetLobbyOpenHost(cs));
                    if (effHostMap &&
                        !(clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP)) {
                        if (ImGui::Button(langGetText(STR_DLGLOBBY_CHOOSE_MAP_BTN))) {
                            lobbyChooseMapOpen(cs, renderer);
                        }
                        ImGui::Spacing();
                        /* Adjust remaining tab height for the button
                         * row we just consumed so the preview below
                         * keeps its aspect ratio. */
                        tabH -= ImGui::GetFrameHeightWithSpacing()
                              + ImGui::GetStyle().ItemSpacing.y;
                    }

                    /* Prefer the existing texture even while we're
                     * waiting on fresh bytes — keeps the panel from
                     * flashing to "Downloading…" between picks when
                     * an older map is still drawable. The rebuild
                     * block atomically swaps it for a fresh texture
                     * once the new bytes arrive. */
                    if (mapPreviewTex) {
                        int pad = 10;   /* extra zoom-out margin so edge-start initials have room */
                        int bx0 = mapBounds.minX - pad; if (bx0 < 0) bx0 = 0;
                        int by0 = mapBounds.minY - pad; if (by0 < 0) by0 = 0;
                        int bx1 = mapBounds.maxX + pad; if (bx1 >= MAP_PREVIEW_SIZE) bx1 = MAP_PREVIEW_SIZE - 1;
                        int by1 = mapBounds.maxY + pad; if (by1 >= MAP_PREVIEW_SIZE) by1 = MAP_PREVIEW_SIZE - 1;
                        int bw = bx1 - bx0;
                        int bh = by1 - by0;
                        if (bw > bh) {
                            int diff = bw - bh;
                            by0 -= diff / 2; by1 += (diff + 1) / 2;
                            if (by0 < 0) { by1 -= by0; by0 = 0; }
                            if (by1 >= MAP_PREVIEW_SIZE) { by0 -= (by1 - MAP_PREVIEW_SIZE + 1); by1 = MAP_PREVIEW_SIZE - 1; }
                            if (by0 < 0) by0 = 0;
                        } else if (bh > bw) {
                            int diff = bh - bw;
                            bx0 -= diff / 2; bx1 += (diff + 1) / 2;
                            if (bx0 < 0) { bx1 -= bx0; bx0 = 0; }
                            if (bx1 >= MAP_PREVIEW_SIZE) { bx0 -= (bx1 - MAP_PREVIEW_SIZE + 1); bx1 = MAP_PREVIEW_SIZE - 1; }
                            if (bx0 < 0) bx0 = 0;
                        }
                        ImVec2 uv0((float)bx0 / MAP_PREVIEW_SIZE, (float)by0 / MAP_PREVIEW_SIZE);
                        ImVec2 uv1((float)(bx1 + 1) / MAP_PREVIEW_SIZE, (float)(by1 + 1) / MAP_PREVIEW_SIZE);

                        float infoH = ImGui::GetTextLineHeightWithSpacing() * 2;
                        float previewMaxH = tabH - infoH;
                        float previewMaxW = ImGui::GetContentRegionAvail().x;
                        float previewSize = previewMaxW < previewMaxH ? previewMaxW : previewMaxH;
                        if (previewSize < 10.0f) previewSize = 10.0f;
                        /* Inset the map by a ~2-tile gap so initials pushed
                         * toward the edges have room to draw around it. */
                        float spanTiles = (float)((bx1 + 1) - bx0);
                        float gapPx = (spanTiles > 0.0f) ? (2.0f * previewSize / spanTiles) : 0.0f;
                        if (gapPx < 30.0f) gapPx = 30.0f;   /* room for edge initials */
                        if (gapPx > previewSize * 0.30f) gapPx = previewSize * 0.30f;
                        float innerSize = previewSize - 2.0f * gapPx;
                        float boxTopY = ImGui::GetCursorPosY();
                        float offsetX = (previewMaxW - innerSize) * 0.5f;
                        if (offsetX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                        ImGui::SetCursorPosY(boxTopY + gapPx);
                        ImVec2 imgScreen = ImGui::GetCursorScreenPos();
                        /* Fill the inset gap with deep-water blue. */
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(imgScreen.x - gapPx, imgScreen.y - gapPx),
                            ImVec2(imgScreen.x + innerSize + gapPx, imgScreen.y + innerSize + gapPx),
                            IM_COL32(0, 0, 80, 255));
                        /* Same magnified 1-px-per-square art as the
                         * two-column path — point-sample it. */
                        imguiPushNearestSampling();
                        ImGui::Image((ImTextureID)mapPreviewTex, ImVec2(innerSize, innerSize), uv0, uv1);
                        imguiPopNearestSampling();
                        ImVec2 miniMin = ImGui::GetItemRectMin();
                        bool miniConsumed = lobbyPreviewInteract(cs, (int)myPlayerNum,
                                                effHostMap, miniMin, innerSize,
                                                bx0, by0, bx1, by1);
                        lobbyDrawPreviewStartOverlay(cs, myPlayerNum, miniMin, innerSize,
                                                     bx0, by0, bx1, by1);
                        /* Controller-reachable entry to the start picker: a
                         * focusable activation over the preview that opens the
                         * popup (which in controller mode shows the start list).
                         * The pad has no click, so the mouse onClick path below
                         * can't reach it; Space/A on this item does. */
                        if (uiShouldUseControllerMode() && lobbyMapPreview()->popupCompressedData) {
                            ImGui::SetCursorScreenPos(miniMin);
                            ImGui::SetNextItemAllowOverlap();
                            if (ImGui::InvisibleButton("##openStartPicker",
                                                       ImVec2(innerSize, innerSize))) {
                                mapPreviewPopupOpenCompressed(lobbyMapPreview()->popupCompressedData,
                                                              lobbyMapPreview()->popupCompressedLen,
                                                              mapBounds.minX, mapBounds.minY,
                                                              mapBounds.maxX, mapBounds.maxY);
                            }
                        }
                        /* Reserve the full box so the gap also sits below. */
                        ImGui::SetCursorPosY(boxTopY + previewSize);
                        /* A click that didn't land on a start opens the zoomed
                         * popup (clicking a free start moves you there). */
                        if (lobbyMapPreview()->popupCompressedData && !miniConsumed &&
                            !uiShouldUseControllerMode()) {
                            mapPreviewPopupOnClick(lobbyMapPreview()->popupCompressedData, lobbyMapPreview()->popupCompressedLen,
                                                   mapBounds.minX, mapBounds.minY,
                                                   mapBounds.maxX, mapBounds.maxY);
                        }
                    } else if (!clientSimIsMapDownloadComplete(cs) || awaitingMapChangePacket) {
                        float progress = 0.0f;
                        ImGui::TextUnformatted(lobbyMapTransferLine(cs, &progress));
                        ImGui::Spacing();
                        ImGui::ProgressBar(progress, ImVec2(-1, 20.0f * s));
                    } else {
                        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MAP_UNAVAILABLE));
                    }
                    ImGui::Spacing();
                    ImGui::Text("%s - %dP %dB %dS", clientSimGetMapName(cs), clientSimGetLobbyPillCount(cs), clientSimGetLobbyBaseCount(cs), clientSimGetLobbyStartCount(cs));

                    /* Skip-map vote is gated by LOBBY_LOCK_MAP — locking
                     * the map blocks both manual change and skip-vote. */
                    if (!spectator && clientSimIsMapSkipAvailable(cs) && clientSimIsInLobby(cs) &&
                        !(clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP)) {
                        ImGui::Spacing();
                        bool countdownActive = clientSimGetCountdownSeconds(cs) > 0;
                        if (countdownActive) ImGui::BeginDisabled();
                        bool voted = clientSimIsMapSkipMyVote(cs);
                        const char *skipLabel = voted ? langGetText(STR_DLGLOBBY_CANCELSKIP) : langGetText(STR_DLGLOBBY_SKIPMAP);
                        if (voted) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.1f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.7f, 0.3f, 0.05f, 1.0f));
                        }
                        if (ImGui::Button(skipLabel, ImVec2(100 * s, 0))) {
                            clientSimSetMapSkipMyVote(cs, !clientSimIsMapSkipMyVote(cs));
                            if (hasTransport) {
                                clientSimNetSendMapSkipVote(cs);
                            }
                        }
                        if (voted) {
                            ImGui::PopStyleColor(3);
                        }
                        ImGui::SameLine();
                        int skipCount = 0, humanCount = 0;
                        for (int j = 0; j < MAX_TANKS; j++) {
                            const ClientLobbySlot *jSlot = clientSimGetLobbySlot(cs, (BYTE)j);
                            if (jSlot && jSlot->connected && !jSlot->isBot) {
                                humanCount++;
                                if (clientSimIsMapSkipVote(cs, (BYTE)j)) skipCount++;
                            }
                        }
                        {
                            MessageArgs args = {};
                            args.number = skipCount;
                            args.number2 = humanCount;
                            ImGui::TextUnformatted(langGetTextFmt(STR_DLGLOBBY_VOTES, &args));
                        }
                        if (countdownActive) ImGui::EndDisabled();
                    }

                    ImGui::EndTabItem();
                }

                /* --- Settings tab (host-only) --- */
                /* The flat game-settings form, no collapsing header — that
                 * chrome belongs to the desktop two-column path. Gated on the
                 * same gsEffectiveHost as the vis[] Settings predicate above. */
                if (gsEffectiveHost &&
                    ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_SETTINGS_HEADER), nullptr,
                        *lobbyPlayersForceTab() == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    activeTab = 2;
                    ImGui::Spacing();
                    lobbyRenderGameSettingsBody(cs, myPlayerNum, s);
                    ImGui::EndTabItem();
                }

                /* --- Chat tab (with unread indicator) --- */
                {
                    bool chatTabColorPushed = false;
                    if (chatUnread) {
                        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.5f, 0.0f, 0.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                        chatTabColorPushed = true;
                    }
                    if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT), nullptr,
                            *lobbyPlayersForceTab() == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                        activeTab = 3;
                        chatUnread = false;
                        if (chatTabColorPushed) {
                            ImGui::PopStyleColor(2);
                            chatTabColorPushed = false;
                        }
                        float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                        float inputH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
                        float chatHistH = tabH - inputH;
                        if (chatHistH < 20.0f) chatHistH = 20.0f;

                        ImGui::BeginChild("##ChatHistory", ImVec2(0, chatHistH), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
                        lobbyRenderChatHistory(clientSimGetLobbyChatHistory(cs));
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                            ImGui::SetScrollHereY(1.0f);
                        }
                        ImGui::EndChild();

                        lobbyRenderChatInputAndSend(cs, chatInput, myPlayerNum, hasTransport, s, 0xFF);

                        ImGui::EndTabItem();
                    }
                    if (chatTabColorPushed) {
                        ImGui::PopStyleColor(2);
                    }
                }

                /* --- Team tab (only when on a team; own unread state) --- */
                {
                    const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                    BYTE myTeam = (!spectator && mySlot) ? mySlot->teamNumber : 0;
                    if (myTeam != 0) {
                        bool teamTabColorPushed = false;
                        if (teamChatUnread) {
                            ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.5f, 0.0f, 0.0f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                            teamTabColorPushed = true;
                        }
                        if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT_TEAM), nullptr,
                                *lobbyPlayersForceTab() == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
                            activeTab = 4;
                            teamChatUnread = false;
                            if (teamTabColorPushed) {
                                ImGui::PopStyleColor(2);
                                teamTabColorPushed = false;
                            }
                            float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                            float inputH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
                            float chatHistH = tabH - inputH;
                            if (chatHistH < 20.0f) chatHistH = 20.0f;

                            ImGui::BeginChild("##TeamChatHistory", ImVec2(0, chatHistH), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
                            lobbyRenderChatHistory(clientSimGetLobbyTeamChatHistory(cs));
                            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                                ImGui::SetScrollHereY(1.0f);
                            }
                            ImGui::EndChild();

                            lobbyRenderChatInputAndSend(cs, chatInput, myPlayerNum, hasTransport, s,
                                                        (BYTE)(CHAT_DEST_TEAM_BASE + myTeam));

                            ImGui::EndTabItem();
                        }
                        if (teamTabColorPushed) {
                            ImGui::PopStyleColor(2);
                        }
                    }
                }

                /* --- Last round tab (only while a summary exists) --- */
                if (haveLastRound &&
                    ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_LASTROUND_BTN), nullptr,
                        *lobbyPlayersForceTab() == 5 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    activeTab = 5;
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                    ImGui::BeginChild("##LastRoundTab", ImVec2(availW, tabH),
                                      ImGuiChildFlags_NavFlattened);
                    lobbyRenderLastRoundBody(cs, s);
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            /* One-shot: the forced selection has been applied (or the bar
             * wasn't drawn this frame), so don't keep re-forcing it. */
            *lobbyPlayersForceTab() = -1;

            /* --- Bottom buttons (always visible) --- */
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                bool myReady = (!spectator && mySlot && mySlot->connected) ? mySlot->ready : false;
                /* Block Ready when the lobby is flagged Ranked but the
                 * shape doesn't qualify. Ranked-ineligibility doesn't
                 * matter in SP / LAN where Ranked isn't shown at all. */
                bool rankedActive = !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs)
                                     && clientSimGetLobbyRanked(cs);
                LobbyRankedEligibility readyRe = rankedActive
                                              ? lobbyComputeRankedEligibility(cs)
                                              : LobbyRankedEligibility{true, 0, 0, 0};
                bool rankedBlocksReady = rankedActive && !readyRe.sizesEligible;
                bool canReady = clientSimIsMapDownloadComplete(cs) && !rankedBlocksReady;

                /* Controller mode draws each action's bound glyph inline, just
                 * left of the button it triggers (A = Ready, B = Leave); the
                 * LT/RT tab-switch glyphs sit at the row's right. Decoration
                 * only — the button keeps its text label if a glyph is absent.
                 * The glyph height matches the button frame height, so the row
                 * height is unchanged (no extra reserved space). */
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

                /* The viewer holds no slot to ready up — hide the Ready
                 * button (Leave below stays available). */
                if (!spectator) {
                if (!canReady) ImGui::BeginDisabled();
                const char *readyLabel = myReady ? langGetText(STR_DLGLOBBY_UNREADY) : langGetText(STR_DLGLOBBY_READY);
                if (myReady) {
                    ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.45f, 0.10f, 1.0f));
                }
                /* A glyph sits left of Ready. Drawn before the focus seed so
                 * SetKeyboardFocusHere() still targets the Button (next item),
                 * not the glyph image. */
                glyphInline(SI_ACTION_MENU_ACCEPT);
                /* One-shot initial focus for controller players — only once
                 * Ready is enabled, so we don't try to focus a disabled item. */
                if (focusReadyPending && canReady) {
                    ImGui::SetKeyboardFocusHere();
                    focusReadyPending = false;
                }
                if (ImGui::Button(readyLabel, ImVec2(100 * s, 0))) {
                    if (hasTransport) {
                        lobbySendReadyToggle(cs, !myReady);
                    }
                }
                if (myReady) ImGui::PopStyleColor(3);
                if (!canReady) ImGui::EndDisabled();
                if (rankedBlocksReady &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    lobbyRankedShapeTooltip(readyRe);
                }
                }  /* close !spectator: Ready button */

                /* Legacy Balance Teams + Apply / Dismiss removed — the
                 * Balance-from-WBN affordance up top is the only entry
                 * point, and the server auto-applies WBN's split on
                 * response (no approval step). */

                ImGui::SameLine(0, 20);
                glyphInline(SI_ACTION_MENU_CANCEL);   /* B glyph left of Leave */
                if (ImGui::Button(langGetText(STR_DLGLOBBY_LEAVE), ImVec2(100 * s, 0)) ||
                    (((ImGui::IsKeyPressed(ImGuiKey_Escape) && !mapPreviewPopupIsOpen() && !s_chooser.open && (uiShouldUseControllerMode() ? (!ImGui::GetIO().WantTextInput && !keyboardIsOpen()) : !dialogNavWasInsideSubRegionAtFrameStart())) ||
                      (ImGui::IsKeyPressed(ImGuiKey_W) && IMGUI_PRIMARY_KEY_DOWN())
#ifdef __APPLE__
                      || (ImGui::IsKeyPressed(ImGuiKey_Period) && ImGui::GetIO().KeySuper)
#endif
                     ) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                    wantLeaveConfirm = true;
                }

                /* LT/RT switch tabs — the two trigger glyphs at the right of
                 * the same button row, no text label (no string fits). */
                if (padLegend) {
                    SDL_Texture *gl = glyphForActionAuto(SI_ACTION_MENU_TAB_LEFT);
                    SDL_Texture *gr = glyphForActionAuto(SI_ACTION_MENU_TAB_RIGHT);
                    if (gl || gr) {
                        ImGui::SameLine(0.0f, 18.0f);
                        if (gl) ImGui::Image((ImTextureID)gl, ImVec2(glyphH, glyphH));
                        if (gl && gr) ImGui::SameLine(0.0f, 2.0f);
                        if (gr) ImGui::Image((ImTextureID)gr, ImVec2(glyphH, glyphH));
                    }
                }
            }
        }
        else {
            /* --- Desktop: Players (left) + Map Preview (right) --- */
            float availW = ImGui::GetContentRegionAvail().x - padR;
            /* Total vertical content area before any rendering — used to
             * fill the lobby so the chat block's bottom sits flush with
             * the lobby's bottom edge (minus the bottom safe inset). */
            float fullContentH = ImGui::GetContentRegionAvail().y;
            /* Map panel: scales to ~30% of available width on larger
             * screens, but never shrinks below the natural preview
             * size (so on small windows the map stays readable and
             * the teams take whatever extra room there is). */
            float mapPanelW = ImMax((MAP_PREVIEW_SIZE + 20) * s,
                                    availW * 0.30f);
            /* Between rounds the right column holds the recap instead of the
             * preview, and a replay reel wants far more width than a map
             * thumbnail — but the players/chat column still has to be worth
             * reading, so it keeps a floor and the recap gives the width back
             * on a narrow lobby. Keyed on the summary, not on which of the
             * panel's two tabs is up: switching tabs must not reflow the
             * lobby around the player. */
            const float kRecapPanelFrac  = 0.55f;
            const float kRecapLeftMinW   = 360.0f;
            if (lobbyShowLastRound) {
                float recapW = ImMax((MAP_PREVIEW_SIZE + 20) * s,
                                     availW * kRecapPanelFrac);
                float roomW  = availW - 8.0f - kRecapLeftMinW * s;
                if (recapW > roomW) recapW = roomW;
                if (recapW > mapPanelW) mapPanelW = recapW;
            }

            /* Draggable split. The width above is the automatic split; the
             * gutter splitter (drawn between the two columns further down)
             * accumulates a signed offset off it, positive widening the left
             * column. Kept as an offset in logical pixels rather than as a
             * fraction so a window resize reflows the automatic part and
             * leaves the user's adjustment where they put it.
             *
             * The drag floors are deliberately looser than the automatic
             * layout's own (kRecapLeftMinW, the natural preview width): this
             * is the user overriding the automatic split, so they only need
             * to be stopped short of squashing either column into nothing.
             * The offset is re-synced to the clamped result each frame, so
             * dragging past a floor doesn't build up slack the user has to
             * drag back out before the split moves again. */
            const float kSplitterW     = 8.0f;
            const float kSplitLeftMinW = 180.0f;
            const float kSplitMapMinW  = 220.0f;
            if (!s_lobbySplitOffsetInit) {
                s_lobbySplitOffsetMap   = gameFrontLobbySplit;
                s_lobbySplitOffsetRecap = gameFrontLobbySplitRecap;
                s_lobbySplitOffsetInit  = true;
            }
            /* Pick the showing view's offset before any width is computed —
             * the panel flips on the next frame (see the ##MapPanel button),
             * so reading it here draws the frame the view changes on at that
             * view's width instead of a frame of the old one. */
            bool   splitOnRecap = lobbyRecapReelVisible(cs);
            float *splitOffset  = splitOnRecap ? &s_lobbySplitOffsetRecap
                                               : &s_lobbySplitOffsetMap;
            float *splitSaved   = splitOnRecap ? &gameFrontLobbySplitRecap
                                               : &gameFrontLobbySplit;
            float autoMapPanelW = mapPanelW;
            mapPanelW -= *splitOffset * s;
            float maxMapW = availW - kSplitterW - kSplitLeftMinW * s;
            if (mapPanelW > maxMapW) mapPanelW = maxMapW;
            /* Map floor last so it wins on a lobby too narrow for both. */
            if (mapPanelW < kSplitMapMinW * s) mapPanelW = kSplitMapMinW * s;
            *splitOffset = (autoMapPanelW - mapPanelW) / (s > 0.0f ? s : 1.0f);

            /* Persist through the same debounced window-settings path the
             * position and size use — a drag or a re-clamp is a change, and
             * gameFrontPumpDirty (already driven per frame below) flushes the
             * trailing one. Only the showing view's value moves; the other
             * keeps whatever it was left at. The epsilon is coarser than the
             * two decimals the value is stored at, so a reload can't look
             * like a change. */
            if (SDL_fabsf(*splitOffset - *splitSaved) > 0.02f) {
                *splitSaved = *splitOffset;
                gameFrontSaveWindowSettings();
            }

            float playerPanelW = availW - mapPanelW - kSplitterW;

            /* "Allow New Players" row spans the full width above both
             * panels so PlayerPanel and MapPanel top edges align in Y. */
            float beforeAllowY = ImGui::GetCursorPosY();
            lobbyRenderAllowNewPlayersRow(cs, myPlayerNum, s);
            float allowRowH = ImGui::GetCursorPosY() - beforeAllowY;

            float spacingH = ImGui::GetStyle().ItemSpacing.y;
            (void)spacingH;
            /* Footer below the map is now exactly one row for the
             * Ready button — the legacy Balance Teams / Apply /
             * Dismiss row was removed (Balance-from-WBN lives next
             * to the Ranked checkbox up top and the server auto-
             * applies WBN's split, so there's nothing extra to fit
             * here any more). The map preview gets the reclaimed
             * vertical space back. */
            float frameH = ImGui::GetFrameHeight();
            float readyAreaH = frameH;

            /* Split the remaining vertical space between PlayerPanel
             * (top) and ChatBlock (bottom). The chat's bottom edge
             * sits flush with the lobby's bottom-content edge (minus
             * the bottom safe inset, used as padding); the right
             * column (MapPanel + Ready row) fills the same vertical
             * span so its bottom edge aligns with the chat's.
             *
             * Splitting policy:
             *   - Chat is CAPPED at 5 visible lines (label + ~3.4
             *     history rows + input row ≈ 5 line heights). Above
             *     that the chat ends up sparse and the team list
             *     wants the space more.
             *   - PlayerPanel takes everything else, so as the
             *     lobby grows the teams section expands while the
             *     chat block stays compact. */
            float lineH = ImGui::GetTextLineHeightWithSpacing();
            float leftFillH = fullContentH - allowRowH - padB - 6.0f;
            if (leftFillH < lineH * 12.0f) leftFillH = lineH * 12.0f;
            /* 7 lines baseline: label (~1) + history (~4) + input row
             * (~1.5) + internal padding. Grow proportionally when the
             * lobby window is taller than a "default" of ~25 line
             * heights — we slide the chat's top up so the user gets a
             * couple more visible history rows on big screens, while
             * still keeping the teams panel as the dominant area.
             *
             * Growth rate is 15% of the surplus, capped at +5 lines
             * (so even on a very tall lobby the chat stays well under
             * half the column). */
            const float kChatBaseline = 7.0f;
            const float kChatDefaultH = 25.0f;
            const float kChatGain     = 0.15f;
            const float kChatMaxExtra = 5.0f;
            float chatCap = lineH * kChatBaseline;
            float surplusLines = (leftFillH / lineH) - kChatDefaultH;
            if (surplusLines > 0.0f) {
                float extra = surplusLines * kChatGain;
                if (extra > kChatMaxExtra) extra = kChatMaxExtra;
                chatCap += lineH * extra;
            }
            float bottomH = chatCap;
            if (bottomH > leftFillH - lineH * 6.0f) {
                bottomH = leftFillH - lineH * 6.0f;
            }
            float panelH = leftFillH - bottomH - spacingH;
            if (panelH < lineH * 4.0f) panelH = lineH * 4.0f;
            float mapH = leftFillH - readyAreaH;
            if (mapH < panelH) mapH = panelH;

            /* Left column — PlayerPanel above ChatBlock, grouped so the
             * right column can SameLine alongside the whole stack. */
            ImGui::BeginGroup();

            /* Left: Player panel — Layout A team-grouped rendering. */
            ImGui::BeginChild("##PlayerPanel", ImVec2(playerPanelW, panelH), ImGuiChildFlags_None);

            /* Layout A: team-grouped player rendering. Legacy 6-column
             * table preserved below the #if 0 for reference; toggle to
             * A/B compare during the in-progress UI rewrite. */
#if 1
            {
                bool isHostHere = lobbyIsHost(cs, myPlayerNum);
                lobbyRenderTeamGroupedPlayers(cs, myPlayerNum, s, isHostHere);
            }
            if (false) {
#else
            if (ImGui::BeginTable("##PlayerTable", 6,
                                  ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY)) {
#endif
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_SLOT_COL),   ImGuiTableColumnFlags_WidthFixed, 30.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_NAME_COL),   ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_PING_COL),   ImGuiTableColumnFlags_WidthFixed, 45.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_TEAM_COL),   ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_READY_COL),  ImGuiTableColumnFlags_WidthFixed, 40.0f * s);
                ImGui::TableSetupColumn(langGetText(STR_DLGLOBBY_ACTION_COL), ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
                ImGui::TableHeadersRow();

                bool botsAllowed = (clientSimGetLobbyAiType(cs) != 0);

                for (int i = 0; i < MAX_TANKS; i++) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", i);

                    const ClientLobbySlot *slot = clientSimGetLobbySlot(cs, (BYTE)i);
                    if (slot && slot->connected) {
                        bool isMe = (i == myPlayerNum);

                        /* Player Name (with flag icon) */
                        ImGui::TableSetColumnIndex(1);
                        if (slot->countryCode[0] != '\0') {
                            if (drawCountryFlagWithTip(slot->countryCode)) {
                                ImGui::SameLine();
                            }
                        }
                        if (!slot->isBot) {
                            uint8_t pflags = slot->clientFlags;
                            if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                                pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
                            }
                            renderPlayerName(NULL,
                                             pflags,
                                             slot->clientType,
                                             "", false);
                        }
                        if (slot->isBot) {
                            MessageArgs args = {};
                            strncpy(args.playerName, slot->playerName, sizeof(args.playerName) - 1);
                            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s",
                                               langGetTextFmt(STR_DLGLOBBY_BOT_FMT, &args));
                        } else if (isMe) {
                            MessageArgs args = {};
                            strncpy(args.playerName, slot->playerName, sizeof(args.playerName) - 1);
                            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s",
                                               langGetTextFmt(STR_DLGLOBBY_YOU_FMT, &args));
                        } else {
                            ImGui::Text("%s", slot->playerName);
                        }

                        /* Ping */
                        ImGui::TableSetColumnIndex(2);
                        if (slot->pingMs > 0) {
                            ImVec4 pingColor =
                                imguiPingBandColor(pingBandClassify(slot->pingMs));
                            ImGui::TextColored(pingColor, "%dms", (int)slot->pingMs);
                        } else {
                            ImGui::TextDisabled("-");
                        }

                        /* Team */
                        ImGui::TableSetColumnIndex(3);
                        if (isMe && hasTransport) {
                            int teamIdx = slot->teamNumber;
                            ImGui::SetNextItemWidth(-1);
                            char comboId[16];
                            SDL_snprintf(comboId, sizeof(comboId), "##team%d", i);
                            if (ImGui::Combo(comboId, &teamIdx, teamItems, 17)) {
                                lobbySendTeamSet(cs, (uint8_t)myPlayerNum, (uint8_t)teamIdx);
                            }
                        } else {
                            if (slot->teamNumber > 0) {
                                ImGui::Text("%d", slot->teamNumber);
                            } else {
                                ImGui::TextDisabled("%s", langGetText(STR_NONE));
                            }
                        }
                        if (clientSimIsBalanceProposalActive(cs) && clientSimGetBalanceProposal(cs, (BYTE)i) != 0 &&
                            clientSimGetBalanceProposal(cs, (BYTE)i) != slot->teamNumber) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "-> %d", clientSimGetBalanceProposal(cs, (BYTE)i));
                        }

                        /* Ready */
                        ImGui::TableSetColumnIndex(4);
                        if (slot->ready) {
                            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "%s", langGetText(STR_YES));
                        } else {
                            ImGui::TextDisabled("%s", langGetText(STR_NO));
                        }

                        /* Action */
                        ImGui::TableSetColumnIndex(5);
                        if (slot->isBot && hasTransport) {
                            char btnId[16];
                            SDL_snprintf(btnId, sizeof(btnId), "Remove##%d", i);
                            if (ImGui::SmallButton(btnId)) {
                                lobbySendRemoveBot(cs, (uint8_t)i);
                            }
                        }
                    } else {
                        /* Empty slot */
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("---");
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextDisabled("-");
                        ImGui::TableSetColumnIndex(5);
                        if (botsAllowed && hasTransport) {
                            char btnId[16];
                            SDL_snprintf(btnId, sizeof(btnId), "Add Bot##%d", i);
                            bool pending = lobbyAddBotPending(cs);
                            if (pending) ImGui::BeginDisabled();
                            if (ImGui::SmallButton(btnId)) {
                                lobbySendAddBotDebounced(cs, -1, 0);
                            }
                            if (pending) ImGui::EndDisabled();
                        }
                    }
                }
                ImGui::EndTable();
            }

            ImGui::EndChild(); /* ##PlayerPanel */

            ImGui::Spacing();

            /* Left bottom: chat block (label + history + input row),
             * still in the left column group so it stacks under
             * PlayerPanel. Rect captured below for the map-chooser
             * scrim's hole-punch. */
            ImVec2 chatBlockCursor = ImGui::GetCursorScreenPos();
            ImGui::BeginChild("##ChatBlock", ImVec2(playerPanelW, bottomH), ImGuiChildFlags_None);
            {
                const ClientLobbySlot *myChatSlot = clientSimGetLobbySlot(cs, myPlayerNum);
                BYTE myTeam = (!spectator && myChatSlot) ? myChatSlot->teamNumber : 0;

                /* Detect new chat messages for the unread indicator. A buffer
                 * that grew while its tab is not the active one flags that tab
                 * red; the flag is cleared inside the tab's own BeginTabItem
                 * block (which only runs for the selected tab), so growth on
                 * the tab you are already viewing clears the same frame and
                 * never flags. Mirrors the mobile tabbed layout. */
                int chatLen = (int)SDL_strlen(clientSimGetLobbyChatHistory(cs));
                if (chatLen > lastChatLen) chatUnread = true;
                lastChatLen = chatLen;
                int teamChatLen = (int)SDL_strlen(clientSimGetLobbyTeamChatHistory(cs));
                if (teamChatLen > lastTeamChatLen) teamChatUnread = true;
                lastTeamChatLen = teamChatLen;

                if (ImGui::BeginTabBar("##ChatTabs")) {
                    /* --- General chat tab (with unread indicator) --- */
                    bool genTabColorPushed = false;
                    if (chatUnread) {
                        ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.5f, 0.0f, 0.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                        genTabColorPushed = true;
                    }
                    bool genTabOpen = ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT_GENERAL));
                    if (genTabColorPushed) {
                        ImGui::PopStyleColor(2);
                        genTabColorPushed = false;
                    }
                    if (genTabOpen) {
                        chatUnread = false;
                        float inputRowH = ImGui::GetFrameHeightWithSpacing();
                        float chatHeight = ImGui::GetContentRegionAvail().y - inputRowH;
                        if (chatHeight < ImGui::GetTextLineHeightWithSpacing() * 3.4f)
                            chatHeight = ImGui::GetTextLineHeightWithSpacing() * 3.4f;
                        ImGui::BeginChild("##ChatHistory", ImVec2(0, chatHeight), ImGuiChildFlags_Borders);
                        lobbyRenderChatHistory(clientSimGetLobbyChatHistory(cs));
                        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                            ImGui::SetScrollHereY(1.0f);
                        }
                        ImGui::EndChild();
                        lobbyRenderChatInputAndSend(cs, chatInput, myPlayerNum, hasTransport, s, 0xFF);
                        ImGui::EndTabItem();
                    }
                    /* --- Team chat tab (only when on a team; own unread state) --- */
                    if (myTeam != 0) {
                        bool teamTabColorPushed = false;
                        if (teamChatUnread) {
                            ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.5f, 0.0f, 0.0f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                            teamTabColorPushed = true;
                        }
                        bool teamTabOpen = ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT_TEAM));
                        if (teamTabColorPushed) {
                            ImGui::PopStyleColor(2);
                            teamTabColorPushed = false;
                        }
                        if (teamTabOpen) {
                            teamChatUnread = false;
                            float inputRowH = ImGui::GetFrameHeightWithSpacing();
                            float chatHeight = ImGui::GetContentRegionAvail().y - inputRowH;
                            if (chatHeight < ImGui::GetTextLineHeightWithSpacing() * 3.4f)
                                chatHeight = ImGui::GetTextLineHeightWithSpacing() * 3.4f;
                            ImGui::BeginChild("##TeamChatHistory", ImVec2(0, chatHeight), ImGuiChildFlags_Borders);
                            lobbyRenderChatHistory(clientSimGetLobbyTeamChatHistory(cs));
                            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f) {
                                ImGui::SetScrollHereY(1.0f);
                            }
                            ImGui::EndChild();
                            lobbyRenderChatInputAndSend(cs, chatInput, myPlayerNum, hasTransport, s,
                                                        (BYTE)(CHAT_DEST_TEAM_BASE + myTeam));
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();
            /* Record chat block rect for the map-chooser scrim. Use the
             * stored cursor position (chatBlockCursor) plus the panel's
             * known size — GetItemRectMin/Max after EndChild has been
             * unreliable on some imgui builds when the child is part
             * of a group. */
            *lobbyChatBlockMin() = chatBlockCursor;
            *lobbyChatBlockMax() = ImVec2(chatBlockCursor.x + playerPanelW,
                                          chatBlockCursor.y + bottomH);

            ImGui::EndGroup(); /* /left column */

            /* Vertical splitter, sized to fill the gutter exactly so the two
             * columns keep landing on availW. SameLine(0,0) on both sides —
             * the gutter is the button, not item spacing. Hit-tested full
             * column height; the offset it drives is clamped where the widths
             * are computed, above. */
            ImGui::SameLine(0, 0.0f);
            ImVec2 splitPos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##LobbyColSplitter",
                                   ImVec2(kSplitterW, leftFillH));
            bool splitActive = ImGui::IsItemActive();
            if (splitActive || ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                /* Only visible while the user is on it — the resting lobby
                 * keeps the plain gap it has always had. */
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(splitPos.x + kSplitterW * 0.5f, splitPos.y),
                    ImVec2(splitPos.x + kSplitterW * 0.5f, splitPos.y + leftFillH),
                    ImGui::GetColorU32(splitActive ? ImGuiCol_SeparatorActive
                                                   : ImGuiCol_SeparatorHovered),
                    2.0f);
            }
            if (splitActive) {
                /* Mouse delta is real pixels; the offset is logical. Moves
                 * only the showing view's offset. */
                *splitOffset +=
                    ImGui::GetIO().MouseDelta.x / (s > 0.0f ? s : 1.0f);
            }
            ImGui::SameLine(0, 0.0f);

            /* Right column — MapPanel extends down to just above the
             * Ready/Balance footer, so the map preview's bottom border
             * sits on the same Y as the Ready button's top. */
            ImGui::BeginGroup();

            /* Right: Map preview + info */
            ImGui::BeginChild("##MapPanel", ImVec2(mapPanelW, mapH), ImGuiChildFlags_Borders);

            /* Between rounds the panel holds two views — the round recap and
             * the map panel, whole and unchanged — and one button across the
             * top swaps between them, captioned with the view it goes to. The
             * map keeps its full panel rather than collapsing to a row because
             * picking next round's start is a mini-map interaction: the start
             * overlay, the click-to-claim and the zoomed picker all live in
             * the preview below, and there is nowhere else to do it before the
             * countdown. With no summary there is no button and showMapPanel
             * stays true, so the panel is exactly the map panel. The child
             * keeps its id so the map chooser's scrim still finds it. */
            bool showMapPanel = !lobbyShowLastRound || *lobbyRecapShowMap();
            if (lobbyShowLastRound) {
                /* The flip lands on the next frame, so the caption and what is
                 * under it always describe the same view. */
                if (ImGui::Button(langGetText(showMapPanel
                                                  ? STR_DLGLOBBY_LASTROUND_BTN
                                                  : STR_DLGLOBBY_MAP_TAB),
                                  ImVec2(-1, 0))) {
                    *lobbyRecapShowMap() = !*lobbyRecapShowMap();
                }
                if (!showMapPanel) {
                    lobbyRenderLastRoundBody(cs, s);
                }
            }

            /* Prefer the existing texture even while we're waiting on
             * fresh bytes — the rebuild block above swaps it
             * atomically once the new map's chunks finish arriving,
             * so users keep seeing the previously-selected map until
             * the new one is ready to slot in. */
            if (showMapPanel && mapPreviewTex) {
                /* Compute UV coordinates to zoom into the interesting area with padding */
                int pad = 10;   /* extra zoom-out margin so edge-start initials have room */
                int bx0 = mapBounds.minX - pad; if (bx0 < 0) bx0 = 0;
                int by0 = mapBounds.minY - pad; if (by0 < 0) by0 = 0;
                int bx1 = mapBounds.maxX + pad; if (bx1 >= MAP_PREVIEW_SIZE) bx1 = MAP_PREVIEW_SIZE - 1;
                int by1 = mapBounds.maxY + pad; if (by1 >= MAP_PREVIEW_SIZE) by1 = MAP_PREVIEW_SIZE - 1;
                /* Make the region square so the preview isn't distorted */
                int bw = bx1 - bx0;
                int bh = by1 - by0;
                if (bw > bh) {
                    int diff = bw - bh;
                    by0 -= diff / 2;
                    by1 += (diff + 1) / 2;
                    if (by0 < 0) { by1 -= by0; by0 = 0; }
                    if (by1 >= MAP_PREVIEW_SIZE) { by0 -= (by1 - MAP_PREVIEW_SIZE + 1); by1 = MAP_PREVIEW_SIZE - 1; }
                    if (by0 < 0) by0 = 0;
                } else if (bh > bw) {
                    int diff = bh - bw;
                    bx0 -= diff / 2;
                    bx1 += (diff + 1) / 2;
                    if (bx0 < 0) { bx1 -= bx0; bx0 = 0; }
                    if (bx1 >= MAP_PREVIEW_SIZE) { bx0 -= (bx1 - MAP_PREVIEW_SIZE + 1); bx1 = MAP_PREVIEW_SIZE - 1; }
                    if (bx0 < 0) bx0 = 0;
                }
                ImVec2 uv0((float)bx0 / MAP_PREVIEW_SIZE, (float)by0 / MAP_PREVIEW_SIZE);
                ImVec2 uv1((float)(bx1 + 1) / MAP_PREVIEW_SIZE, (float)(by1 + 1) / MAP_PREVIEW_SIZE);

                /* Size the preview square as (M - N) - small margin, where
                 *   M = total panel inner vertical space available now
                 *   N = total vertical height of every other thing that
                 *       will render in this panel (Choose Map button,
                 *       separator, the 4-line info block, optional
                 *       Skip Map vote row).
                 * Pre-measure N so the preview can claim everything else
                 * deterministically and the panel doesn't end up with
                 * either dead space or content pushed past the bottom. */
                bool isHostLocal  = lobbyIsHost(cs, myPlayerNum);
                bool isAdminLocal = (myPlayerNum < MAX_TANKS &&
                    (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                     & PLAYER_FLAG_ADMIN));
                bool effHostMap = !spectator && (isHostLocal || isAdminLocal ||
                                  clientSimGetLobbyOpenHost(cs));
                bool skipAvail = clientSimIsMapSkipAvailable(cs)
                              && clientSimIsInLobby(cs);

                float M       = ImGui::GetContentRegionAvail().y;
                float lineH   = ImGui::GetTextLineHeightWithSpacing();
                float frameH  = ImGui::GetFrameHeight();
                float spcH    = ImGui::GetStyle().ItemSpacing.y;

                float N = 0.0f;
                /* Choose Map button (right under the preview) */
                if (effHostMap) N += frameH + spcH;
                /* Spacing + Separator + Spacing */
                N += spcH + 1.0f + spcH;
                /* Map / pillboxes / bases / starts — 4 text rows */
                N += 4.0f * lineH;
                /* Skip Map button row (button + same-line votes text) */
                if (skipAvail) N += spcH + frameH;

                float panelWidth = ImGui::GetContentRegionAvail().x;
                float previewSize = M - N - 6.0f;  /* small breathing room */
                if (previewSize > panelWidth) previewSize = panelWidth;
                if (previewSize < 64.0f)     previewSize = 64.0f;
                /* Inset the map by a ~2-tile gap inside the reserved box so
                 * initials pushed toward the edges have room to draw around
                 * it. Image + overlay shrink together, keeping dots aligned. */
                float spanTiles = (float)((bx1 + 1) - bx0);
                float gapPx = (spanTiles > 0.0f) ? (2.0f * previewSize / spanTiles) : 0.0f;
                if (gapPx < 30.0f) gapPx = 30.0f;   /* room for edge initials */
                if (gapPx > previewSize * 0.30f) gapPx = previewSize * 0.30f;
                float innerSize = previewSize - 2.0f * gapPx;
                float boxTopY = ImGui::GetCursorPosY();
                float offsetX = (panelWidth - innerSize) * 0.5f;
                if (offsetX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
                ImGui::SetCursorPosY(boxTopY + gapPx);
                ImVec2 imgScreen = ImGui::GetCursorScreenPos();
                /* Fill the inset gap with deep-water blue (matches the minimap
                 * DEEP_SEA colour) so the surround reads as sea, not panel. */
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(imgScreen.x - gapPx, imgScreen.y - gapPx),
                    ImVec2(imgScreen.x + innerSize + gapPx, imgScreen.y + innerSize + gapPx),
                    IM_COL32(0, 0, 80, 255));
                /* 1 px per map square cropped to the bounding box and blown
                 * up over the panel — a 5-7x magnification that bilinear
                 * turns to mush. Point-sample it; the surrounding UI goes
                 * back to LINEAR straight after. */
                imguiPushNearestSampling();
                ImGui::Image((ImTextureID)mapPreviewTex, ImVec2(innerSize, innerSize), uv0, uv1);
                imguiPopNearestSampling();
                ImVec2 miniMin = ImGui::GetItemRectMin();
                bool miniConsumed = lobbyPreviewInteract(cs, (int)myPlayerNum,
                                        effHostMap, miniMin, innerSize,
                                        bx0, by0, bx1, by1);
                lobbyDrawPreviewStartOverlay(cs, myPlayerNum, miniMin, innerSize,
                                             bx0, by0, bx1, by1);
                /* Reserve the full box so the gap also sits below the map. */
                ImGui::SetCursorPosY(boxTopY + previewSize);
                /* A click that didn't land on a start opens the zoomed popup
                 * (clicking a free start moves you there instead). Mouse only;
                 * this two-column layout is never used in controller mode. */
                if (lobbyMapPreview()->popupCompressedData && !miniConsumed &&
                    !uiShouldUseControllerMode()) {
                    mapPreviewPopupOnClick(lobbyMapPreview()->popupCompressedData, lobbyMapPreview()->popupCompressedLen,
                                           mapBounds.minX, mapBounds.minY,
                                           mapBounds.maxX, mapBounds.maxY);
                }

                /* Choose Map button — sits directly under the preview so
                 * the "change map" affordance reads as part of the
                 * preview block rather than as a footer at the bottom.
                 * Hidden when LOBBY_LOCK_MAP is set (server pins map). */
                if (effHostMap &&
                    !(clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP)) {
                    if (ImGui::Button(langGetText(STR_DLGLOBBY_CHOOSE_MAP_BTN), ImVec2(-1, 0))) {
                        lobbyChooseMapOpen(cs, renderer);
                    }
                }
            } else if (showMapPanel && (!clientSimIsMapDownloadComplete(cs) ||
                                        awaitingMapChangePacket)) {
                /* No texture yet AND we're mid-download — show the
                 * progress bar so the user knows something's coming. */
                float progress = 0.0f;
                ImGui::TextUnformatted(lobbyMapTransferLine(cs, &progress));
                ImGui::Spacing();
                ImGui::ProgressBar(progress, ImVec2(-1, 20.0f * s));
                ImGui::Spacing();
            } else if (showMapPanel) {
                ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MAP_UNAVAILABLE));
            }

            if (showMapPanel) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                /* Map info — show the lock badge inline with the map name
                 * when LOBBY_LOCK_MAP is set so admins / non-hosts can see
                 * the map is pinned even though the Choose Map / Skip-Map
                 * affordances aren't drawn. */
                ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MAP_LBL), clientSimGetMapName(cs));
                if ((clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP) != 0) {
                    ImGui::SameLine(0.0f, 4.0f * s);
                    lobbyRenderLockBadge();
                }
                ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_PILLBOXES), clientSimGetLobbyPillCount(cs));
                ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_BASES), clientSimGetLobbyBaseCount(cs));
                ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_STARTS), clientSimGetLobbyStartCount(cs));

                lobbyRenderMapSkipVote(cs, spectator, hasTransport, s, false);
            }

            /* Choose Map button moved up to sit directly under the
             * preview image (see the mapPreviewTex branch above). The
             * pre-measured N height for the preview-sizing math
             * accounts for it. */

            ImGui::EndChild(); /* ##MapPanel */

            /* Ready / Balance buttons sit directly below MapPanel inside
             * the right column group. The MapPanel height was sized so
             * its bottom border lands just above this footer, aligning
             * with the top of the Ready button. */
            {
                const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                bool myReady = (!spectator && mySlot && mySlot->connected) ? mySlot->ready : false;
                /* Block Ready when the lobby is flagged Ranked but the
                 * shape doesn't qualify. Ranked-ineligibility doesn't
                 * matter in SP / LAN where Ranked isn't shown at all. */
                bool rankedActive = !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs)
                                     && clientSimGetLobbyRanked(cs);
                LobbyRankedEligibility readyRe = rankedActive
                                              ? lobbyComputeRankedEligibility(cs)
                                              : LobbyRankedEligibility{true, 0, 0, 0};
                bool rankedBlocksReady = rankedActive && !readyRe.sizesEligible;
                bool canReady = clientSimIsMapDownloadComplete(cs) && !rankedBlocksReady;

                /* No legacy Balance Teams button in the right column —
                 * the Balance-from-WBN affordance lives next to the
                 * Ranked checkbox up top, and the server auto-applies
                 * WBN's split immediately on response (no Apply /
                 * Dismiss approval step). */

                /* The viewer holds no slot to ready up — hide the Ready button. */
                if (!spectator) {
                if (!canReady) ImGui::BeginDisabled();
                const char *readyLabel = myReady ? langGetText(STR_DLGLOBBY_UNREADY) : langGetText(STR_DLGLOBBY_READY);
                if (myReady) {
                    ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.45f, 0.10f, 1.0f));
                }
                /* One-shot initial focus for controller players — only once
                 * Ready is enabled, so we don't try to focus a disabled item. */
                if (focusReadyPending && canReady) {
                    ImGui::SetKeyboardFocusHere();
                    focusReadyPending = false;
                }
                if (ImGui::Button(readyLabel, ImVec2(-1, 0))) {
                    if (hasTransport) {
                        lobbySendReadyToggle(cs, !myReady);
                    }
                }
                if (myReady) ImGui::PopStyleColor(3);
                if (!canReady) ImGui::EndDisabled();
                if (rankedBlocksReady &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    lobbyRankedShapeTooltip(readyRe);
                }
                }  /* close !spectator: Ready button */
            }
            ImGui::EndGroup(); /* /right column */
        } /* /desktop layout scope (playerPanelW/mapPanelW) */

        /* --- Map preview popup --- */
        {
            /* Local edit-authority — drives both whether the "Change"
             * button renders inside the popup and whether the chooser
             * actually opens on a Change request. "Allow players to
             * change game settings" (openHost) extends this beyond
             * the host slot to every connected player. */
            bool isHostLocal  = lobbyIsHost(cs, myPlayerNum);
            bool isAdminLocal = (cs && myPlayerNum < MAX_TANKS &&
                (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                 & PLAYER_FLAG_ADMIN));
            bool effHostMap = !spectator && (isHostLocal || isAdminLocal ||
                              (cs && clientSimGetLobbyOpenHost(cs)));
            /* Only offer "Choose map" in the popup when the user may
             * actually change it — same gate as the inline Choose Map
             * button (hidden when the server pins the map). */
            bool mapChangeAllowed = effHostMap &&
                !(cs && (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP));
            mapPreviewPopupSetShowChange(mapChangeAllowed);
            mapPreviewPopupSetStartPicker(cs, spectator ? -1 : myPlayerNum, effHostMap);
            mapPreviewPopupRenderModal(renderer);
            if (mapPreviewPopupConsumeChangeRequest() && mapChangeAllowed) {
                lobbyChooseMapOpen(cs, renderer);
            }
        }

        /* --- Leave confirmation popup --- */
        char leavePopupModalId[64];
        SDL_snprintf(leavePopupModalId, sizeof(leavePopupModalId), "%s##lobby", langGetText(STR_DLGLOBBY_LEAVE_TITLE));
        static bool s_llOpen = true; s_llOpen = true;
        if (ImGui::BeginPopupModal(leavePopupModalId, &s_llOpen,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_LEAVE_BLURB));
            /* [No (stay)] [Yes (leave)] — Yes is the destructive primary; No
             * is the cancel-equivalent. Reordering keeps the localized Yes/No
             * labels while matching the [Cancel][Confirm] spec convention.
             * No Enter-confirm: destructive action requires an explicit click. */
            int f = WBUI::DialogFooter(langGetText(STR_NO),
                                       langGetText(STR_YES));
            if (f == WBUI::FOOTER_CONFIRM) {
                ImGui::CloseCurrentPopup();
                leftLobby = true;
            } else if (f == WBUI::FOOTER_CANCEL) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        /* Open the leave confirm one frame after the trigger fires, so the
         * modal first renders on a frame where the B/Escape press has already
         * been released. Otherwise DialogFooter's own Escape-cancel inside the
         * modal consumes the same press and dismisses it on appear. */
        if (wantLeaveConfirm)
            ImGui::OpenPopup(leavePopupModalId);

        /* --- Countdown overlay --- */
        if (clientSimGetNetStatus(cs) == netLobbyCountdown && clientSimGetCountdownSeconds(cs) > 0) {
            char countdownText[64];
            MessageArgs args = {};
            args.number = clientSimGetCountdownSeconds(cs);
            SDL_snprintf(countdownText, sizeof(countdownText), "%s",
                         langGetTextFmt(STR_DLGLOBBY_STARTING_FMT, &args));
            ImGui::PushFont(countdownFont);
            ImVec2 textSize = ImGui::CalcTextSize(countdownText);
            ImGui::PopFont();
            ImVec2 winPos = ImGui::GetWindowPos();
            ImVec2 winSize = ImGui::GetWindowSize();
            ImVec2 textPos = ImVec2(
                winPos.x + (winSize.x - textSize.x) * 0.5f,
                winPos.y + (winSize.y - textSize.y) * 0.5f
            );
            ImDrawList *fg = ImGui::GetForegroundDrawList();
            float pad = 12.0f * s;
            fg->AddRectFilled(
                ImVec2(textPos.x - pad, textPos.y - pad * 0.5f),
                ImVec2(textPos.x + textSize.x + pad, textPos.y + textSize.y + pad * 0.5f),
                IM_COL32(0, 0, 0, 180), 6.0f * s);
            fg->AddText(countdownFont, countdownFontSize, textPos,
                        IM_COL32(255, 255, 0, 255), countdownText);
        }

        ImGui::End(); /* ##LobbyBg */
        ImGui::PopStyleVar(); /* WindowPadding */

        /* Scrim behind the map chooser: darkens + absorbs input over
         * the whole lobby, EXCEPT a hole over the chat block so the
         * chat stays usable while picking / generating a map.
         *
         * Two windows do it without needing a polygon cut-out:
         *   1. Top:        (0, 0) .. (winW, chatMin.y)
         *   2. Bottom-right: (chatMax.x, chatMin.y) .. (winW, winH)
         * Together they cover everything outside the chat hole
         * (the chat is at the bottom-left of the lobby and reaches
         * the screen bottom, so we don't need a fourth strip below).
         *
         * Each scrim is a borderless borderless window with a custom
         * dark background colour and a full-area InvisibleButton to
         * eat the mouse click. NoBringToFrontOnFocus + NoFocusOnAppearing
         * keep it from stealing focus from the chooser, which is
         * rendered just below this block (so it draws on top in Z). */
        if (s_chooser.open &&
            lobbyChatBlockMax()->x > lobbyChatBlockMin()->x &&
            lobbyChatBlockMax()->y > lobbyChatBlockMin()->y) {
            /* NoBringToFrontOnFocus / NoFocusOnAppearing keep the z-order
             * DETERMINISTIC by creation order rather than focus: ##LobbyBg
             * (also flagged NoBringToFrontOnFocus) is begun first, then these
             * scrims, then the chooser — so LobbyBg < scrims < chooser holds
             * every frame. Without this it relied on focus-follows-input,
             * which is reliable only in the desktop lobby's private ImGui
             * context; in the WASM shared context a scrim could rise above the
             * chooser and its ##scrimHit absorbed every click (the chooser
             * looked open but was dead). The scrim still draws above the
             * background because it is created after ##LobbyBg. */
            const ImGuiWindowFlags scrimFlags =
                ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoFocusOnAppearing;
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 140));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

            auto drawScrim = [&](const char *id, float x, float y,
                                 float w, float h) {
                if (w <= 0.0f || h <= 0.0f) return;
                ImGui::SetNextWindowPos(ImVec2(x, y));
                ImGui::SetNextWindowSize(ImVec2(w, h));
                ImGui::Begin(id, NULL, scrimFlags);
                ImGui::InvisibleButton("##scrimHit", ImVec2(w, h));
                ImGui::End();
            };

            /* Top strip — above the chat. */
            drawScrim("##chooserScrimTop", 0.0f, 0.0f,
                      (float)winW, lobbyChatBlockMin()->y);
            /* Right strip — right of the chat, full remaining height. */
            drawScrim("##chooserScrimRight", lobbyChatBlockMax()->x,
                      lobbyChatBlockMin()->y,
                      (float)winW - lobbyChatBlockMax()->x,
                      (float)winH - lobbyChatBlockMin()->y);

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            /* Focus the chooser on the frame it OPENS so it draws above
             * the scrim windows we just begun. Doing this every frame
             * snatches focus back from anything the user clicks into the
             * chat-hole (the chat InputText loses its cursor on the very
             * next frame). Once the chooser is on top, ImGui's natural
             * focus follows the click. */
            if (!s_chooser.focusedOnce) {
                ImGui::SetNextWindowFocus();
                s_chooser.focusedOnce = true;
            }
        } else {
            /* Reset the edge-trigger so re-opening focuses again. */
            s_chooser.focusedOnce = false;
        }

        /* Map chooser sub-window (Phase 1). Rendered after the main
         * lobby End() so it's a top-level ImGui window that the user
         * can drag around freely. Only renders when s_chooser.open is
         * true, set by the "Choose Map" button on the Map tab.
         * Pass the *live* winW/winH (updated each frame from
         * SDL_GetWindowSize) — screenW/screenH is cached at lobby
         * entry and doesn't track OS-window resizes. */
        lobbyChooseMapRenderWindow(cs, renderer, s, winW, winH);

#if !BOLO_MOBILE
    /* The reel outlives any single body render. Once the summary is gone (the
     * countdown clears it) drop the reel and the per-summary load latch —
     * unconditionally, since a round whose load failed leaves the latch set
     * with no reel to end, and would otherwise block every later round in the
     * session. lobbyReelEnd is idempotent, and so is the blob drop that goes
     * with it — a download that completed after the recap it belongs to went
     * away is stale, and the round starting now will ask for its own. While a
     * summary stands, freeze a reel the recap stopped drawing — a tab switched
     * away from must not leave the decoder ticking unseen. */
    if (!clientSimGetLastRoundStats(cs)) {
        lobbyReelDropRoundLog(cs);
        lobbyReelEnd();
    } else if (s_reel.active && !s_reel.drawn && lvEmbedIsPlaying()) {
        lvEmbedPause();
        s_reel.autoPaused = true;
    }
    s_reel.drawn = false;
#endif

    if (leftLobby) return LOBBY_FRAME_LEFT;
    return LOBBY_FRAME_CONTINUE;
}

/* Usable bounds of the display a restored lobby window should be fitted
 * to: the one the saved dialog position lands on, else the one the window
 * is currently on, else the primary. Mirrors the same fallback chain the
 * game window's restore uses in winbolo.c. False when SDL can't name a
 * display at all, in which case the caller skips the clamp rather than
 * clamping against garbage. */
static bool lobbyRestoreUsableBounds(SDL_Window *window, SDL_Rect *out) {
    SDL_DisplayID dispID = 0;
    if (gameFrontDialogX >= 0 && gameFrontDialogY >= 0) {
        SDL_Point pt = { gameFrontDialogX, gameFrontDialogY };
        dispID = SDL_GetDisplayForPoint(&pt);
    }
    if (!dispID && window) dispID = SDL_GetDisplayForWindow(window);
    if (!dispID) dispID = SDL_GetPrimaryDisplay();
    if (!dispID) return false;
    return SDL_GetDisplayUsableBounds(dispID, out);
}

/* Record the lobby window's current size and mark the window settings
 * dirty. Debounced downstream (gameFrontSaveWindowSettings writes at most
 * once per 500ms, gameFrontPumpDirty flushes the trailing event), so this
 * is safe to call from every move/resize event of a drag.
 *
 * Skipped when the window size isn't the player's to choose: controller
 * mode leaves the host window alone, and an active device preset forces
 * its own dimensions — saving either would overwrite the desktop size. */
static void lobbySaveWindowGeometry(SDL_Window *window) {
    if (!window) return;
#if !BOLO_MOBILE
    if (uiShouldUseControllerMode()) return;
    if (g_currentDevicePreset >= 0 && g_currentDevicePreset < s_numDevicePresets &&
        s_devicePresets[g_currentDevicePreset].mode != UI_MODE_DESKTOP) {
        return;
    }
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0) return;
    gameFrontLobbyW = w;
    gameFrontLobbyH = h;
    gameFrontSaveWindowSettings();
#else
    (void)window;
#endif
}

/* Blocking desktop modal: owns a private ImGui context + SDL backends and
 * runs its own event/draw loop, calling imguiLobbyRenderFrame() to build
 * each frame. Returns 1 if the game started, 0 if the player left. */
extern "C" int imguiLobbyShow(ClientSim *cs) {
    WB_LOG_INFO(WB_LOG_CAT_GUI, "[LOBBY] imguiLobbyShow called cs=%p inLobby=%d netStat=%d isSP=%d",
            (void*)cs, cs ? (int)clientSimIsInLobby(cs) : -1, cs ? (int)clientSimGetNetStatus(cs) : -1,
            cs ? (int)clientSimIsSinglePlayer(cs) : -1);
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return 0;

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    /* Get screen size and compute UI scale */
    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);
#if !BOLO_MOBILE
    if (!uiModeIsSteamDeck()) s = 1.0f;   /* lobby + nested map chooser / start picker: desktop scaling deferred to the lobby rework */
#endif

#if !BOLO_MOBILE
    /* Reopen at the size the player last left the lobby at, falling back to
     * the built-in default when nothing is saved. Clamped to the usable
     * bounds of the display the restore targets so a size saved on a bigger
     * monitor that is no longer attached can't come back larger than the
     * screen. dialogSetWindowSize still owns the controller-mode and
     * device-preset overrides. */
    {
        int lobbyW = DIALOG_W, lobbyH = DIALOG_H;
        if (gameFrontLobbyW > 0 && gameFrontLobbyH > 0) {
            lobbyW = gameFrontLobbyW;
            lobbyH = gameFrontLobbyH;
        }
        SDL_Rect usable;
        if (lobbyRestoreUsableBounds(window, &usable)) {
            if (lobbyW > usable.w) lobbyW = usable.w;
            if (lobbyH > usable.h) lobbyH = usable.h;
        }
        if (lobbyW < DIALOG_MIN_W) lobbyW = DIALOG_MIN_W;
        if (lobbyH < DIALOG_MIN_H) lobbyH = DIALOG_MIN_H;
        dialogSetWindowSize(window, lobbyW, lobbyH);
    }
    dialogSetWindowTitle(window, langGetText(STR_DLGLOBBY_WINTITLE));
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
    /* A saved position from a monitor that has since been unplugged (or one
     * that no longer fits the restored size) would leave the lobby off-screen
     * with no way to drag it back, so pull it inside the target display's
     * usable area. Only writes when it actually moved, so the normal case
     * leaves the saved position untouched. */
    {
        SDL_Rect usable;
        int px = 0, py = 0, ww = 0, wh = 0;
        SDL_GetWindowPosition(window, &px, &py);
        SDL_GetWindowSize(window, &ww, &wh);
        if (lobbyRestoreUsableBounds(window, &usable) && ww > 0 && wh > 0) {
            int cx = px, cy = py;
            if (cx + ww > usable.x + usable.w) cx = usable.x + usable.w - ww;
            if (cy + wh > usable.y + usable.h) cy = usable.y + usable.h - wh;
            if (cx < usable.x) cx = usable.x;
            if (cy < usable.y) cy = usable.y;
            if (cx != px || cy != py) {
                SDL_SetWindowPosition(window, cx, cy);
                dialogSaveCurrentPosition(window);
                gameFrontSaveWindowSettings();
            }
        }
    }
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    /* Set up ImGui context */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
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

    /* Load a large font for the countdown overlay */
    float countdownFontSize = (s <= 1.05f) ? 54.0f : 60.0f * s;
    ImFont *countdownFont = imguiLoadBoloFontSized(countdownFontSize);

    /* Release anything a stray in-game-lobby seam frame left behind
     * (lobbyFrameInitState below NULLs mapPreviewTex without destroying
     * it, so an undisposed texture would leak). Idempotent when clean. */
    imguiLobbyFrameReset();

    /* Seed per-frame state, then override the chrome with this private
     * context's computed scale / loaded font / window insets. */
    lobbyFrameInitState(cs);
    s_lf.s                 = s;
    s_lf.countdownFont     = countdownFont;
    s_lf.countdownFontSize = countdownFontSize;
    s_lf.safeInsets        = dialogGetSafeInsets(window);
    s_lf.active            = true;

    int result = 0;
    bool running = true;

    /* Show the lobby in Steam immediately on entry; the throttled tick
     * inside imguiLobbyRenderFrame keeps the player count / connect
     * address current. */
    gameFrontSetSteamPresenceLobby(cs);

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            /* Position goes to gameFrontDialogX/Y (shared with every other
             * dialog — it is the one dialog window); the size is the lobby's
             * own. Both then take the debounced save path. */
            dialogHandleWindowMoveResize(window, &ev);
            if ((ev.type == SDL_EVENT_WINDOW_MOVED ||
                 ev.type == SDL_EVENT_WINDOW_RESIZED) &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                lobbySaveWindowGeometry(window);
            }
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        /* Tick transport to receive lobby packets */
        bool hasTransport = clientSimHasTransport(cs);
        if (hasTransport) {
            clientSimNetTick(cs);
        }

        /* Check for game start */
        if (clientSimGetNetStatus(cs) == netRunning) {
            /* SP jumps straight to running with no real-time countdown, so it
             * gets no countdown cues (gated above); suppress the start noise
             * too for a silent SP entry. MP still plays it. */
            if (!clientSimIsSinglePlayer(cs)) {
                soundPlayEffect(lobbyGameStart);
            }
            result = 1;
            running = false;
            break;
        }

        /* A spectator never reaches netRunning: the server unsubscribes it
         * from the live lobby bus before the running phase is published, so
         * its mode bit flips out of live-lobby. Treat that flip as the
         * game-start signal (result 1, same as a player's game start). */
        if (clientSimIsSpectator(cs) && !clientSimSpectatorIsLiveLobby(cs)) {
            result = 1;
            running = false;
            break;
        }

        /* Check for server disconnect/shutdown */
        if (hasTransport) {
            ClientConnectState js = clientSimGetConnectState(cs);
            if (js == CLIENT_CONNECT_SERVER_SHUTDOWN ||
                js == CLIENT_CONNECT_ERROR ||
                js == CLIENT_CONNECT_KICKED) {
                bool kicked = (js == CLIENT_CONNECT_KICKED);
                imguiMessageBoxEx(DIALOG_BOX_TITLE,
                    langGetText(kicked
                                ? STR_DLGLOBBY_KICKED
                                : STR_DLGLOBBY_LOSTCONNECTION),
                    kicked ? IMGUI_MSG_NONE : IMGUI_MSG_ERROR,
                    IMGUI_MSG_OK);
                result = 0;
                running = false;
                break;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();
        controllerDialogsRenderMenu();

        switch (imguiLobbyRenderFrame(cs)) {
            case LOBBY_FRAME_LEFT:
                result = 0;
                running = false;
                break;
            default:
                break;
        }

        dialogDrawNavOutline();
        keyboardUpdate();   /* controller text entry for this dialog's fields */
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        gameFrontPumpDirty(); /* sync cloud prefs from menus (login join + debounced upload) */
        dialogFrameCapEnd(frameCapStart);
    }

    /* Release per-frame state (texture, popup buffers, transient flags). */
    imguiLobbyFrameReset();

    /* The lobby closing is the last chance to write a move / resize / split
     * drag that landed inside the debounce window — there is no further
     * per-frame pump to flush it. */
    gameFrontFlushWindowSettings();

    /* Dismiss soft keyboard and tear down ImGui */
    dialogDismissKeyboard(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    /* Restore logical presentation */
    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}
