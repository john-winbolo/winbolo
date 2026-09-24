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
 * Name:          lobby_wbn_maps.cpp
 * Purpose:       The lobby map chooser's Winbolo.net Maps
 *                tab: the lobby's side of the shared WBN map
 *                source in wbn_map_source.cpp, which owns the
 *                catalogue cache, the listing and preview
 *                callbacks and the map-download worker. Here
 *                are the two callbacks that need the sim —
 *                the map click handler that submits a
 *                download, and the per-frame tick that drains
 *                a finished one and decides what it is for.
 *                Picking a map in a network lobby sends it to
 *                the host over the regular map upload; a
 *                single-player host has no server to delegate
 *                to, so the bytes are applied to the local
 *                sim on the UI thread.
 *********************************************************/

#include <cstring>  /* strncmp — the "wbn:" prefix test */
#include <string>   /* std::string — the sanitised map name */

#include <SDL3/SDL.h>

/* LobbyChooserState and lobbyChooser — previewPending, raised when a
 * download has fired the live preview; lobbyChooserWbnTab — the chooser
 * instance the downloaded map bytes are pushed into. */
#include "lobby_internal.h"
#include "../../wbn_map_source.h"  /* WbnMapDownloadResult, wbnMapSourceSubmitDownload / PollDownload */
extern "C" {
#include "imgui_mapchooser.h"  /* MapChooserState, mapChooserSetSelectedMapBytes */
#include "client_sim.h"  /* ClientSim / clientSimIsSinglePlayer and the lobby WBN preview + map upload status accessors */
#include "client_net.h"  /* clientSimNetSendLobbyMapUploadBytes — the picked map's bytes on their way to the host */
#include "global.h"      /* MAP_STR_SIZE — the display name buffer */
#include "../../../lang.h"  /* langGetText / STR_DLGLOBBY_WBN_ERR_* */
#include "../../../../common/wb_log.h"  /* WB_LOG_INFO / WB_LOG_WARN / WB_LOG_CAT_GUI — the [MAPPICK], [WBN-SP] and [WBN-MP] trace */
#include "../../../../bolo/public/wire_limits.h"  /* LOBBY_MAP_UPLOAD_MAX_BYTES — ceiling on a map sent to the host */
}

/* The WBN map source is absent from the WASM build (no WBN HTTP
 * backend), and so is this tab. */
#ifndef __EMSCRIPTEN__

/* onSelect for the WBN provider. Synthetic "wbn:<id>" paths only —
 * anything else is a folder click the chooser handled internally.
 * Submitting the id kicks the download worker (which also
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
        wbnMapSourceSubmitDownload(mapId);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[MAPPICK] wbn submitted mapId=%u previewStatus=1",
                    (unsigned)mapId);
    } else {
        WB_LOG_WARN(WB_LOG_CAT_GUI,
                    "[MAPPICK] wbn parse failed for '%s'", sel);
    }
}

/* Main-thread completion handler for a picked map. Drains the
 * download and applies it: SP installs the bytes on the local sim,
 * MP feeds them into the regular MAP_UPLOAD protocol. Called once
 * per frame while the WBN tab is visible. */
static void lobbySpWbnConsume(ClientSim *cs, SDL_Renderer *renderer) {
    if (!cs) return;
    WbnMapDownloadResult res;
    if (!wbnMapSourcePollDownload(&res)) return;
    if (!res.ok) {
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
            lobbyChooserWbnTab(), renderer,
            reinterpret_cast<const uint8_t *>(res.bytes.data()),
            (int)res.bytes.size(), displayName.c_str());
    }

    /* MP host: chunked upload state machine on the transport.
     * SP host: the wrapper's local-transport branch installs the
     * bytes synchronously onto spServerSim (no chunked transfer). */
    if (!clientSimIsSinglePlayer(cs)) {
        /* Either kind: a script the Mods chooser is sending holds the one
           upload the transport runs at a time just as a map does. */
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
    lobbyChooser()->previewPending = true;
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

/* WBN tick: drain any completed background download. Always called
 * on the UI thread; SP applies bytes in-process, MP feeds them into
 * the regular MAP_UPLOAD protocol. */
void lobbyWbnMapsTick(MapChooserState *state, SDL_Renderer *renderer,
                              void *ctx) {
    (void)state;
    ClientSim *cs = (ClientSim *)ctx;
    if (cs) lobbySpWbnConsume(cs, renderer);
}
#endif /* __EMSCRIPTEN__ — WBN Maps tab support code */
