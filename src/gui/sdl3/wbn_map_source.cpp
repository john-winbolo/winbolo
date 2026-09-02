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
 * Name:          wbn_map_source.cpp
 * Purpose:       The WinBolo.net map catalogue as a map
 *                chooser source. Holds the cached catalogue
 *                — the current folder's breadcrumbs,
 *                subfolders and maps, the friendly-path to
 *                folder-id table and a separate
 *                search-result cache — together with the
 *                detached worker threads that fetch a folder
 *                listing or a search from the WinBolo.net
 *                REST API and the cJSON parsing that fills
 *                the cache from their responses. Also the
 *                provider callbacks a chooser drives its
 *                browser widget through — the listing, the
 *                folder click handler and the path tooltip —
 *                plus the preview generator that downloads
 *                and decodes a map's catalogue image, and
 *                the map-download worker: a picked map's
 *                bytes are fetched on their own thread and
 *                parked in a result slot for the host to
 *                drain on the UI thread. Shared by every
 *                chooser that browses the catalogue; the
 *                host decides what a downloaded map is for.
 *********************************************************/

#include "wbn_map_source.h"

#include <cstdlib>  /* free — the JSON and map bytes the WBN HTTP API hands back */
#include <cstring>  /* memset / memcpy — chooser row init and the decoded preview copy */
#include <atomic>   /* std::atomic — the in-flight flags and the supersede sequence numbers */
#include <mutex>    /* std::mutex / std::lock_guard — the caches the workers write under */
#include <thread>   /* std::thread — the detached folder, search and map-download workers */
#include <string>   /* std::string — folder paths, map names and error text */
#include <vector>   /* std::vector — the cached listing, the search results and the downloaded bytes */
#include <map>      /* std::map — the friendly path to folder id table */
#include <utility>  /* std::move / std::pair — cache handoff and the breadcrumb prefix list */

#include <SDL3/SDL.h>

extern "C" {
#include "dialogs/imgui_mapchooser.h"  /* MapChooserState / MapChooserEntry / MapPreviewPixels, MAP_CHOOSER_MAX_MAPS, mapChooserEmitFolderRowsForSearchHits */
#include "cJSON.h"       /* the WBN REST responses are parsed with cJSON */
#include "../lang.h"     /* langGetText / langGetTextFmt / MessageArgs / STR_DLGLOBBY_WBN_ERR_* */
#include "../../common/wb_log.h"  /* WB_LOG_INFO / WB_LOG_WARN / WB_LOG_CAT_GUI — the [WBN-TAB] and [WBN-SP] trace */
#include "../../winbolonet/http.h"  /* httpGetBaseUrl / wbn_api_get / wbn_api_download_to_memory[_cancellable] */

/* stb_image entry points used by wbnMapsGeneratePreview (defined in
 * src/third_party/stb/stb_image_impl.c). */
unsigned char *stbi_load_from_memory(const unsigned char *, int,
                                     int *, int *, int *, int);
void stbi_image_free(void *);
}

/* ── Winbolo.net catalogue — state and async folder fetch ─────────
 * Everything here depends on the libcurl-backed WBN HTTP API
 * (httpGetBaseUrl / wbn_api_get / wbn_api_download_to_memory*), which
 * the WASM build does not link. The chooser tab that uses it is
 * already runtime-gated on winbolonetIsRunning() (false in WASM), so
 * it is dead-but-linked there; compile it out entirely to keep
 * wasm-ld's undefined-symbol set clean.
 *
 * The catalogue is the WBN REST API. Folder listings and search
 * results are fetched on a detached std::thread; the UI thread
 * renders from a mutex-guarded parsed snapshot. */
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
typedef struct WbnMapsCache {
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
} WbnMapsCache;

static WbnMapsCache s_wbnMaps;

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

/* ── Map download ────────────────────────────────────────────────
 * A picked map's bytes are fetched by this client process on a
 * worker thread (mirrors the server's PREVIEW_WBN handler) and parked
 * in a result slot for the host to drain on the main thread via
 * wbnMapSourcePollDownload. Cancel-on-supersede uses the http.c
 * cancellable API so rapid map clicks abort the in-flight curl call. */
struct WbnDownloadResult {
    uint32_t mapId;
    int      httpStatus; /* 200 ok; -2 cancelled */
    std::string mapName;
    std::vector<uint8_t> bytes;
    std::string err;
    bool     valid;
};

typedef struct WbnDownloadState {
    std::mutex             mutex;
    std::atomic<bool>      fetching{false};
    std::atomic<uint32_t>  fetchSeq{0};
    volatile int           cancel = 0; /* CURLOPT_XFERINFO sink */
    WbnDownloadResult      result;
} WbnDownloadState;

/* Cleared a field at a time by wbnMapSourceResetDownload and never as a
 * whole: the mutex and the two atomics are not assignable, and the worker
 * is detached so there is no join that would make a whole-struct write
 * safe. */
static WbnDownloadState s_wbnDownload;

/* Drop a download the host is walking away from, so a map's bytes do not
 * sit in the result slot until the next pick drains it. The worker is
 * detached and cannot be stopped, so the fetch is superseded the same way
 * wbnMapSourceSubmitDownload supersedes one: the cancel flag aborts curl
 * mid transfer and the seq bump makes any completion that still lands get
 * dropped. The flag stays raised — every attempt lowers it for itself
 * before the transfer starts. fetching belongs to the worker, which clears
 * it on its way out whichever branch it takes. */
void wbnMapSourceResetDownload(void) {
    s_wbnDownload.cancel = 1;
    ++s_wbnDownload.fetchSeq;
    {
        std::lock_guard<std::mutex> lk(s_wbnDownload.mutex);
        s_wbnDownload.result = WbnDownloadResult{};
    }
}

void wbnMapSourceSubmitDownload(uint32_t mapId) {
    /* Supersede any in-flight call. The cancel flag aborts curl;
     * the seq bump invalidates the completion. */
    s_wbnDownload.cancel = 1;
    uint32_t seq = ++s_wbnDownload.fetchSeq;
    /* Drop any undrained previous result. */
    {
        std::lock_guard<std::mutex> lk(s_wbnDownload.mutex);
        s_wbnDownload.result = WbnDownloadResult{};
    }
    std::thread([mapId, seq]() {
        /* Wait briefly for previous thread to clear the fetching
         * flag — both threads race the same flag. */
        for (int i = 0; i < 50 && s_wbnDownload.fetching.load(); i++) {
            SDL_Delay(10);
        }
        s_wbnDownload.fetching.store(true);
        s_wbnDownload.cancel = 0; /* reset for this attempt */

        WbnDownloadResult out;
        out.mapId = mapId;

        char infoPath[64];
        SDL_snprintf(infoPath, sizeof(infoPath), "maps/info/%u",
                     (unsigned)mapId);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] info fetch: /api/v1/%s", infoPath);
        char *infoJson = nullptr;
        int infoStatus = wbn_api_get(infoPath, &infoJson);
        if (seq != s_wbnDownload.fetchSeq.load()) {
            free(infoJson);
            s_wbnDownload.fetching.store(false);
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
            filePath, &bytes, &bytesLen, &s_wbnDownload.cancel);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] file fetch result: /api/v1/%s -> %d (%zu bytes)",
                    filePath, dlStatus, bytesLen);

        if (seq != s_wbnDownload.fetchSeq.load() || dlStatus == -2) {
            free(bytes);
            s_wbnDownload.fetching.store(false);
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
            std::lock_guard<std::mutex> lk(s_wbnDownload.mutex);
            s_wbnDownload.result = std::move(out);
        }
        s_wbnDownload.fetching.store(false);
    }).detach();
}

/* Main-thread drain of the result slot. Hands the completion back
 * exactly as the worker left it; whether the bytes are usable is
 * summarised in ok. */
bool wbnMapSourcePollDownload(WbnMapDownloadResult *out) {
    if (!out) return false;
    WbnDownloadResult res;
    {
        std::lock_guard<std::mutex> lk(s_wbnDownload.mutex);
        if (!s_wbnDownload.result.valid) return false;
        res = std::move(s_wbnDownload.result);
        s_wbnDownload.result = WbnDownloadResult{};
    }
    out->ok      = (res.httpStatus == 200 && !res.bytes.empty());
    out->mapName = std::move(res.mapName);
    out->bytes   = std::move(res.bytes);
    out->err     = std::move(res.err);
    return true;
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
 * host's click handler parses to trigger the download flow.
 * Sub-folder rows use the friendly child path so a click writes
 * that back into state->currentDir and the cycle repeats. */
void wbnMapsListProvider(MapChooserState *state,
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

/* onFolderJump for the WBN provider. The chooser prepends "Maps" as
 * the clickable root indicator. "Maps" alone (or "Maps/") jumps back
 * to the WBN catalogue root; anything else is a friendly path the
 * provider resolves via s_wbnMaps.pathToId on the next frame. */
void wbnMapsOnFolderJump(MapChooserState *state,
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
bool wbnMapsGeneratePreview(const char *entryPath,
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

/* Tooltip on the WBN path label = the WBN host URL so the user can
 * tell at a glance which server the catalogue is coming from. */
void wbnMapsTooltipPrefix(MapChooserState *state, void *ctx) {
    (void)ctx;
    const char *wbnHost = httpGetBaseUrl();
    SDL_strlcpy(state->pathTooltipPrefix,
                (wbnHost && *wbnHost) ? wbnHost : "winbolo.net",
                sizeof(state->pathTooltipPrefix));
}
#endif /* __EMSCRIPTEN__ — WBN map source support code */
