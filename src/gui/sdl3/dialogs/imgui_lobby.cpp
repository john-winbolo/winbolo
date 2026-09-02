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
    } else if (lobbyReel()->active && !lobbyReel()->drawn && lvEmbedIsPlaying()) {
        lvEmbedPause();
        lobbyReel()->autoPaused = true;
    }
    lobbyReel()->drawn = false;
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
