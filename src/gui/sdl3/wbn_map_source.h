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
 * Name:          wbn_map_source.h
 * Purpose:       The WinBolo.net map catalogue as a map
 *                chooser source. The cached folder listing
 *                and search results the REST API is browsed
 *                through, the provider callbacks a chooser
 *                instance is wired to (listing, folder jump,
 *                path tooltip and catalogue-image preview),
 *                and the worker that downloads a picked
 *                map's bytes for the caller to drain on the
 *                UI thread. Knows nothing about who is
 *                consuming the map — the lobby applies the
 *                bytes to its sim or uploads them; any other
 *                chooser host does its own thing with them.
 *                C++ only, and absent from the WASM build
 *                (no WBN HTTP backend there).
 *********************************************************/

#ifndef WBN_MAP_SOURCE_H
#define WBN_MAP_SOURCE_H

#ifndef __EMSCRIPTEN__

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "dialogs/imgui_mapchooser.h"  /* MapChooserState / MapPreviewPixels — the provider callback signatures */
}

/* ── Chooser provider callbacks ──────────────────────────────────
 * Wire these into a MapChooserState's provider to browse the WBN
 * catalogue. The listing lands asynchronously on a worker thread, so
 * the instance wants refreshEveryFrame to surface cache updates. Map
 * rows carry a synthetic "wbn:<id>" path; the host's onSelect parses
 * it and submits the id below. */
void wbnMapsListProvider(MapChooserState *state,
                         const char *relPath, void *ctx);
void wbnMapsOnFolderJump(MapChooserState *state,
                         const char *jumpPath, void *ctx);
bool wbnMapsGeneratePreview(const char *entryPath,
                            MapPreviewPixels *outBuf,
                            void *ctx);
void wbnMapsTooltipPrefix(MapChooserState *state, void *ctx);

/* ── Map download ────────────────────────────────────────────────
 * One worker slot: submitting a map supersedes whatever is in flight,
 * and the completion sits in the slot until the host polls it off on
 * the UI thread. */

/* One completed map download drained from the worker slot. */
struct WbnMapDownloadResult {
    bool                 ok;      /* HTTP 200 and non-empty bytes */
    std::string          mapName; /* raw name from the catalogue, unsanitised */
    std::vector<uint8_t> bytes;   /* the .map file bytes */
    std::string          err;     /* error text when !ok; may be empty */
};

/* Kick a download of catalogue map mapId on a detached worker. Any
 * earlier download still running is cancelled and its completion
 * dropped. */
void wbnMapSourceSubmitDownload(uint32_t mapId);

/* Drains a completed download if one is waiting. Returns false if
 * nothing has completed. */
bool wbnMapSourcePollDownload(WbnMapDownloadResult *out);

/* Drop a download the host is walking away from: cancels the transfer,
 * invalidates its completion and empties the result slot. */
void wbnMapSourceResetDownload(void);

#endif /* __EMSCRIPTEN__ */

#endif /* WBN_MAP_SOURCE_H */
