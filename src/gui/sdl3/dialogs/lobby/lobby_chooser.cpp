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
 * Name:          lobby_chooser.cpp
 * Purpose:       The lobby's map-chooser window, opened
 *                from the Map tab and drawn as its own
 *                draggable ImGui window over the lobby.
 *                Holds the four chooser instances — Server
 *                Maps, Upload, Generate and Winbolo.net —
 *                whose folder position, selection and
 *                search filter persist across re-opens, the
 *                shared per-tab render helper that drives
 *                one instance through its provider, the tab
 *                bar and its controller shoulder-cycle, the
 *                maximized variant of the window, the
 *                action bar and the close confirmation.
 *                Also the Server Maps provider, which
 *                routes its listing, search and preview
 *                through the server (in-process ServerSim
 *                for an SP host, packets for a network
 *                client), and the Upload provider, which
 *                browses the local filesystem and either
 *                reloads the picked map directly or streams
 *                it up.
 *********************************************************/

#include <cstdio>     /* FILE / fopen / fwrite / fclose, FILENAME_MAX — the preview spill file and the breadcrumb jump buffer */
#include <cstdlib>    /* free — the map bytes serverSimReadMapFile hands back */
#include <cstring>    /* memset / strncmp — row init and the "data/maps/" prefix strip */
#include <cfloat>     /* FLT_MAX — no upper bound on the window size, and no wrap width for the error overlay measure */
#include <algorithm>  /* std::sort — the Server Maps listing order */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImGuiContext — the Esc path reads NavId off the context, which imgui.h only forward-declares */
/* LobbyChooserState and lobbyChooser / lobbyChooserReset; the WinBolo.net
 * tab's sim-facing provider callbacks (lobbyWbnMapsOnSelect / Tick); and
 * through imgui_dialog_utils.h imguiPush/PopNearestSampling and
 * dialogNavWasInsideSubRegionAtFrameStart. */
#include "lobby_internal.h"
#include "../../wbn_map_source.h"  /* wbnMapsListProvider / OnFolderJump / TooltipPrefix / GeneratePreview — the WinBolo.net tab's catalogue callbacks */
#include "dialog_footer.h"  /* WBUI::CancelKeyPressed / DialogFooter3 / FOOTER_* */
extern "C" {
#include "imgui_mapchooser.h"  /* MapChooserState / MapChooserEntry / MapPreviewPixels, MAP_CHOOSER_MAX_MAPS and the mapChooser* API */
#include "client_sim.h"  /* ClientSim + the lobby map list / search / preview / upload getters; UPLOAD_POLICY_OFF */
#include "client_net.h"  /* clientSimNetSendLobby* — list, search, set map, preview, upload, commit, cancel */
#include "server_sim.h"  /* ServerSim / ServerMapEntry; serverSimEnumerateMapDir / SearchMapDir / ReadMapFile / ReloadMap / ReloadRandomMap / CommitPreview / RevertPreview */
#include "../../imgui_steam_nav.h"     /* imguiSteamNavConsumeMenuTabShift — the controller tab cycle */
#include "../../map_preview_view.h"    /* MapPreviewInputOpts + mapPreviewView* — the maximized preview pane */
#include "../../minimap_render.h"      /* minimapRenderPixels / MINIMAP_SIZE — the provider thumbnails */
#include "../../../ui_mode.h"          /* uiShouldUseControllerMode */
#include "../../../lang.h"             /* langGetText / STR_DLGLOBBY_* / STR_MAPCHOOSER_EVERARD */
#include "../../../gamefront.h"        /* gameFrontGetServerSim / GetSinglePlayerServerSim */
#include "../../../../common/wb_log.h"                  /* WB_LOG_INFO / WB_LOG_WARN / WB_LOG_CAT_GUI — the [MAPPICK] trace */
#include "../../../../server/threads.h"                 /* threadsWaitForMutex / Release — SP-host server calls */
#include "../../../../bolo/public/client_mappreview.h"  /* MapPreview / clientMapPreviewLoadFromFile / Destroy */
#if !defined(__ANDROID__) && !defined(__IPHONEOS__) && !defined(__EMSCRIPTEN__)
extern "C" {
#include "../../../../scenario/scenario_host.h"  /* scenarioHostWorkshopDir — the Local Maps tab's Workshop folder */
}
#endif
}

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
    /* Last clientSimGetLobbyMapListSeq the Server Maps tab enumerated on.
     * The listing a network client shows arrives over the wire, so the rows
     * change with no user action on this tab; comparing against the live
     * value re-runs the provider on that edge instead of every frame. */
    uint32_t        serverMapSeq  = 0;
    bool            inited        = false;
    int             activeTab     = 0; /* 0=server 1=upload 2=random 3=wbn */
} LobbyChooserTabs;

static LobbyChooserTabs s_chooserTabs = {};

#ifndef __EMSCRIPTEN__
/* wbnmaps drives the WinBolo.net tab's browser through this, which is why the
 * enclosing LobbyChooserTabs does not have to be published. */
MapChooserState *lobbyChooserWbnTab(void) {
    return &s_chooserTabs.wbn;
}
#endif /* __EMSCRIPTEN__ */

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
                hits, MAP_CHOOSER_MAX_MAPS, true);
            if (got < 0) got = 0;
        } else if (cs && clientSimHasTransport(cs)) {
            /* The search reply carries no scripted byte, so these rows
             * are never tagged; the chooser greys out "Scenarios only"
             * rather than let it hide every hit. */
            state->searchRowsUntagged = true;
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
                    hits[got].scripted = false;
                    got++;
                }
            }
        }
        for (int i = 0; i < got && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
            MapChooserEntry *e = &state->maps[state->numMaps++];
            memset(e, 0, sizeof(*e));
            e->isFolder = hits[i].isFolder;
            e->modTime  = hits[i].modTime;
            e->scripted = hits[i].scripted;
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
                serverEntries[got].scripted =
                    clientSimGetLobbyMapListScripted(cs, i);
                got++;
            }
        }
    }

    for (int i = 0; i < got && state->numMaps < MAP_CHOOSER_MAX_MAPS; i++) {
        MapChooserEntry *e = &state->maps[state->numMaps++];
        memset(e, 0, sizeof(*e));
        e->isFolder = serverEntries[i].isFolder;
        e->modTime  = serverEntries[i].modTime;
        e->scripted = serverEntries[i].scripted;
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
         * compressed format (one zlib stream over the bases/pills/
         * starts structs and the map tiles); we have the raw
         * BMAPBOLO file bytes. mapRead is
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
     * dropped the blob with nothing left to re-request it: the PREVIEW_REQ
     * goes out on the row click, and the transport resends it only while no
     * stream has arrived, so nothing asks again once the blob is delivered.
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
        /* Either kind: a script the Mods chooser is sending holds the one
           upload the transport runs at a time just as a map does. */
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
 * both. Bare "Maps" lands at data/maps itself. The breadcrumb shows the
 * Workshop directory as "Workshop", so "Workshop" and "Workshop/<rest>"
 * go back to that directory, unless data/maps holds a real Workshop
 * folder, which the root lists in its place. */
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
    SDL_PathInfo realWorkshop;
    if (state->workshopDir[0] != '\0' &&
        (SDL_strcmp(jp, "Workshop") == 0 ||
         SDL_strncmp(jp, "Workshop/", 9) == 0) &&
        !(SDL_GetPathInfo("data/maps/Workshop", &realWorkshop) &&
          realWorkshop.type == SDL_PATHTYPE_DIRECTORY)) {
        if (jp[8] == '/') {
            SDL_snprintf(state->currentDir, sizeof(state->currentDir),
                         "%s/%s", state->workshopDir, jp + 9);
        } else {
            SDL_strlcpy(state->currentDir, state->workshopDir,
                        sizeof(state->currentDir));
        }
    } else if (jp[0] == '\0') {
        SDL_strlcpy(state->currentDir, kRootBare, sizeof(state->currentDir));
    } else if (strncmp(jp, kRoot, sizeof(kRoot) - 1) == 0 ||
               SDL_strcasecmp(jp, kRootBare) == 0) {
        SDL_strlcpy(state->currentDir, jp, sizeof(state->currentDir));
    } else {
        SDL_snprintf(state->currentDir, sizeof(state->currentDir),
                     "data/maps/%s", jp);
    }
}

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
    } else if (state == &s_chooserTabs.server) {
        /* Server Maps is the one tab whose rows can change with nothing
         * happening here: on a network client the listing, the recursive
         * search results and the effect of a finished upload all arrive
         * asynchronously. clientSimGetLobbyMapListSeq moves on each of
         * those, so one enumerate per move covers them; the provider's own
         * send-if-the-cache-does-not-match logic issues the request when
         * that enumerate finds a stale cache, and the reply's tick brings
         * us back here once to show it. */
        ClientSim *listCs = (ClientSim *)state->provider.ctx;
        uint32_t   seq = listCs ? clientSimGetLobbyMapListSeq(listCs) : 0;
        if (seq != s_chooserTabs.serverMapSeq) {
            s_chooserTabs.serverMapSeq = seq;
            mapChooserRefresh(state);
        }
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
            /* The status is shared with a script sent from the Mods
               chooser, whose refusal is that dialog's to show. */
            if (!clientSimIsSinglePlayer(cs) && clientSimHasTransport(cs) &&
                clientSimGetLobbyUploadKind(cs) == UPLOAD_KIND_MAP &&
                clientSimGetLobbyMapUploadStatus(cs) == 4) {
                switch (clientSimGetLobbyMapUploadRejectCode(cs)) {
                    case 4: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_INFLIGHT);
                    case 5: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_DISABLED);
                    case 6: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_FULL);
                    case 7: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_COOLDOWN);
                    case 9: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_TIMEOUT);
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
                clientSimGetLobbyUploadKind(cs) == UPLOAD_KIND_MAP &&
                clientSimGetLobbyMapUploadStatus(cs) == 4) {
                switch (clientSimGetLobbyMapUploadRejectCode(cs)) {
                    case 4: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_INFLIGHT);
                    case 5: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_DISABLED);
                    case 6: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_FULL);
                    case 7: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_COOLDOWN);
                    case 9: return langGetText(STR_DLGLOBBY_UPLOAD_ERR_TIMEOUT);
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
        /* No per-frame enumerate here. The in-process branch of the
         * provider scans the map directory and tests every file for a
         * script beside it, which is not something to repeat at frame
         * rate; the network branch is driven instead by the map-list
         * counter lobbyRenderMapTab watches, which moves once per
         * completed reply. */
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
#if !defined(__ANDROID__) && !defined(__IPHONEOS__) && !defined(__EMSCRIPTEN__)
        /* The directory this computer copies its Workshop items to, listed
           as a "Workshop" folder at the root. Mobile has no Workshop. */
        {
            char workshopDir[FILENAME_MAX];
            if (scenarioHostWorkshopDir(workshopDir, sizeof(workshopDir))) {
                mapChooserSetWorkshopDir(&s_chooserTabs.upload, workshopDir);
            }
        }
#endif
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
        /* Both tabs' rows say whether a map has a script beside it, so
         * both offer the "Scenarios only" filter. Winbolo.net rows never
         * say, so that tab does not. */
        s_chooserTabs.server.offerScenariosOnly = true;
        s_chooserTabs.upload.offerScenariosOnly = true;
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
        s_chooserTabs.wbn.provider.enumerate            = wbnMapsListProvider;
        s_chooserTabs.wbn.provider.onSelect             = lobbyWbnMapsOnSelect;
        s_chooserTabs.wbn.provider.onFolderJump         = wbnMapsOnFolderJump;
        s_chooserTabs.wbn.provider.refreshTooltipPrefix = wbnMapsTooltipPrefix;
        s_chooserTabs.wbn.provider.tick                 = lobbyWbnMapsTick;
        s_chooserTabs.wbn.provider.generatePreview      = wbnMapsGeneratePreview;
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
    /* Re-enumerate Server Maps on every open. EnsureInit's discover runs
     * once per process, so without this a second opening of the dialog
     * would show whatever the directory held the first time — and the
     * selection match below reads the rows this produces. Seeding the
     * watched counter from the cs that is bound now keeps a rebound cs
     * (its counters back at zero) from looking like "no change". */
    s_chooserTabs.serverMapSeq = cs ? clientSimGetLobbyMapListSeq(cs) : 0;
    mapChooserRefresh(&s_chooserTabs.server);

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
