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
#include "imgui_nav_outline.h"
#include "imgui_server_address.h"
#include "dialog_footer.h"
#include "nanosvg.h"
#include "nanosvgrast.h"
#include "../imgui_steam_nav.h"

extern "C" {
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "global.h"
#include "bolo_rand.h"
#include "client_sim.h"
#include "client_net.h"
#include "server_sim.h"          /* serverSim* T1 wrappers for SP-host paths */
#include "lobby_bot_pools.h"     /* lobbyBotPool* — public utility */
#include "playername_validate.h" /* playerNameValidate — client-side bot name gate */
#include "../../../server/server_lifecycle.h"
#include "../../../server/threads.h"  /* threadsWaitForMutex / Release — SP-host server calls */
#include "platform_net.h"
#include "../../../common/mp_diag_log.h"
#include "../flags.h"
#include "../sdl3imgui.h"
#include "../../ui_mode.h"
#include "../minimap_render.h"
#include "../../../bolo/public/client_mappreview.h"
#include "../../../bolo/public/wire_limits.h"
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
#include "../../lang.h"
#include "imgui_lobby.h"
#include "imgui_keyboard.h"
#include "imgui_messagebox.h"
#include "imgui_mapchooser.h"
#include "../../../winbolonet/http.h"
#include "../../../winbolonet/winbolonet_core.h"  /* winbolonetIsRunning() — gates WBN-only UI */
#include "cJSON.h"
}
#include "../wb_theme.h"
#include "../lobby_start_markers.h"  /* shared start-ownership marker helpers */

#define MAX_TANKS 16
#define WBN_ICON_SIZE 14
#define CHAT_INPUT_SIZE 129  /* 128 chars + null terminator */
#define MAP_PREVIEW_SIZE 256

static const int DIALOG_W = 1024;
static const int DIALOG_H = 768;

/* Per-slot tracking of whether the bot's name was manually overridden
 * by the host typing into the name input field.  Cleared when a bot
 * is added or its name is rerolled (those are pool-driven names);
 * set when the user types a name in the AiConfig sub-panel.  Used by
 * the team-naming-pool dropdown to decide which bots to auto-rename
 * when the pool changes — overridden names stay, pool-driven names
 * get a fresh pick from the new pool.
 *
 * UI-side state only (no server propagation). For multiplayer the
 * server picks names so this array would never gate anything; for
 * single-player it's the source of truth. */
static bool s_botNameOverridden[MAX_TANKS] = {0};

/* ── Single-player vs multiplayer command dispatch ─────────────────
 * The lobby UI was originally written against the UDP transport — it
 * sends PACKET_LOBBY_* commands and relies on the server broadcasting
 * resulting state changes back via PACKET_LOBBY_STATE.  The single-
 * player path runs the server in-process and has no packet flow, so
 * commands route directly to the local spServerSim and the result is
 * the matching transportUdpServerBroadcast*Chg function fires —
 * those publish ControlEvents on the in-process subscriber bus so
 * the local humanSim sees the new state via clientSimApplyControl.
 *
 * Each helper takes both the ClientSim (to detect single-player) and
 * the Transport pointer (used by the multiplayer path).  When the
 * Transport is null and we're not single-player, the call is a no-op.
 */
static void lobbySendReadyToggle(ClientSim *cs, bool ready) {
    /* Both single-player and multiplayer now go through the same
     * ready toggle. The server's all-ready detector trips the
     * lobby→running transition (synchronously via StartGameInPlace
     * when worldPreLoaded, via countdown+StartGame otherwise).
     * Rich presence is driven by the lobby/game loops' throttled
     * gameFrontTickSteamPresence* refreshers, so there's nothing to
     * push from here. */
    clientSimNetSendReady(cs, ready);
}

/* Sticky "last picked brain" catalogue index. ADD BOT uses this
 * when in range so new bots inherit whatever brain the host last
 * selected (via the per-bot AiConfig "Bot Code" dropdown), instead
 * of always falling back to the server's default brain. 0xFF means
 * "no sticky yet — use the server default"; valid values index into
 * the lobby brain catalogue. Process-scoped. */
static uint8_t s_lastChosenBrainIdx = 0xFF;

/* Add Bot. namingPool < 0 means "use the slot's team pool" (multiplayer
 * server already picks based on team membership). namingPool >= 0
 * forces a specific pool — used by per-team header "+ Bot" buttons
 * which have their own pool dropdown.  teamNumber > 0 assigns the
 * new bot to that team after creation; teamNumber == 0 leaves it on
 * whatever default team the server picked.  Single-player honors both
 * overrides locally; multiplayer currently ignores them (server-side
 * picks the name and default team — TODO: extend the protocol with a
 * teamId-aware add-bot packet). */
static void lobbySendAddBot(ClientSim *cs,
                            int namingPool, uint8_t teamNumber) {
    /* Validate the sticky brain pick against the current catalogue:
     * an out-of-range sticky (e.g. catalogue shrunk between picks)
     * falls back to the server-default sentinel. */
    uint8_t stickyBrainIdx = s_lastChosenBrainIdx;
    if (stickyBrainIdx != 0xFF && cs) {
        const BrainList *bl = clientSimGetLobbyBrainList(cs);
        if (!bl || stickyBrainIdx >= bl->count) stickyBrainIdx = 0xFF;
    }
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        /* Find first free slot */
        BYTE slot;
        for (slot = 1; slot < MAX_TANKS; slot++) {
            if (!serverSimIsPlayerConnected(sim, slot)) break;
        }
        if (slot >= MAX_TANKS) return;
        if (serverSimGetBotBrainPath(sim)[0] == '\0') return;

        /* Pick a name. If a pool override is supplied we use it; else
         * fall back to "Bot N". Build the used-names list from current
         * lobby slots so the picker doesn't collide with existing bots. */
        char botName[32];
        if (namingPool >= 0 && namingPool < lobbyBotPoolCount()) {
            const char *usedNames[MAX_TANKS];
            int usedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (serverSimIsPlayerConnected(sim, i)) {
                    usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
                }
            }
            lobbyBotPoolPick(namingPool, usedNames, usedCount, botName, sizeof(botName));
        } else {
            snprintf(botName, sizeof(botName), "Bot %d", slot);
        }

        /* Mirror the server's bot-name validator before applying.  The
         * picked name is normally a pool / "Bot N" pick — server-trusted
         * by construction — but the gate is consistent with the wire rule
         * (transport_udp_server.c rejects invalid names) and stops a pool
         * definition that smuggled in a control byte, reserved leading
         * '*', etc. from landing in the local sim. */
        if (botName[0] != '\0') {
            char validated[PACKET_MAX_PLAYER_NAME];
            PlayerNameValidationError nameErr = PLAYER_NAME_OK;
            if (!playerNameValidate(botName, validated,
                                    sizeof(validated), &nameErr)) {
                return;
            }
            SDL_strlcpy(botName, validated, sizeof(botName));
        }

        /* Serialise against the SDL timer thread's serverInstanceTick.
         * The publish flag inside serverSim is single-thread; without the
         * mutex the lobby heartbeat in serverInstanceTick can re-enter
         * publishControl while this thread is mid-publish. */
        threadsWaitForMutex();
        serverSimCreateBot(sim, slot, serverSimGetBotBrainPath(sim), botName,
                           (aiType)serverSimGetBotAiType(sim),
                           (gameType)clientSimGetLobbyGameType(cs),
                           clientSimIsLobbyHiddenMines(cs));
        /* Mirror the per-bot brain selection so the AiConfig combo
         * reflects "this bot's brain" rather than a global default.
         * stickyBrainIdx == 0xFF picks up the server's CLI-configured
         * default brain; any other value swaps in the catalogue entry
         * the host last picked. */
        if (stickyBrainIdx != 0xFF) {
            serverSimSwitchBotBrain(sim, slot, stickyBrainIdx);
        } else {
            serverSimSetBotBrainIdxFor(sim, slot, 0xFF);
        }
        /* Publish the slot's new state. serverSimSetBotBrainIdxFor
         * (called via either branch above) publishes the bot-brain
         * event itself. */
        serverSimPublishLobbySlot(sim, slot);
        threadsReleaseMutex();
        /* Bot's name came from the pool — not an override. */
        s_botNameOverridden[slot] = false;
        if (teamNumber > 0 && teamNumber < MAX_TANKS) {
            clientSimNetSendTeamSet(cs, slot, teamNumber);
        }
        return;
    }
    /* MP path */
    {
        /* Pick the bot's name from the chosen pool right here on the
         * client — server doesn't know pool contents (see
         * lobby_bot_pools.h). Empty string falls back to "Bot N". */
        char botName[32];
        botName[0] = '\0';
        if (cs && namingPool >= 0 && namingPool < lobbyBotPoolCount()) {
            const char *usedNames[MAX_TANKS];
            int usedCount = 0;
            for (int i = 0; i < MAX_TANKS; i++) {
                if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected &&
                    clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
                    usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
                }
            }
            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             botName, sizeof(botName));
        }
        /* Same defensive gate as the SP branch.  Empty botName is the
         * legitimate "let the server pick a default" signal and must
         * pass through unchanged. */
        if (botName[0] != '\0') {
            char validated[PACKET_MAX_PLAYER_NAME];
            PlayerNameValidationError nameErr = PLAYER_NAME_OK;
            if (!playerNameValidate(botName, validated,
                                    sizeof(validated), &nameErr)) {
                return;
            }
            SDL_strlcpy(botName, validated, sizeof(botName));
        }
        /* Pass the sticky brain index for API symmetry with the SP path,
         * but be aware: PACKET_LOBBY_ADD_BOT does not propagate it to the
         * server today (the wire payload's brain bytes are vestigial and
         * the parser ignores them). The AiConfig combo issues a follow-up
         * SET_BOT_BRAIN once the new slot lands when a non-default brain
         * is desired. */
        clientSimNetSendAddBotConfigured(cs, teamNumber, stickyBrainIdx, botName);
    }
}

/* Add-bot debounce — disables the Add Bot button while a previously-sent
 * request is in flight. Counts current connected lobby slots at click
 * time, locks the button until either (a) the connected count grows to
 * the expected value (server acked) or (b) a 2 s timeout elapses (lost
 * packet / SP path that already returned). Prevents spam-clicks from
 * pushing past MAX_TANKS or otherwise racing the server.
 *
 * Belt-and-suspenders against the lobby-spam crash:
 *   - s_addBotFrame: ImGui frame number of the last successful send.
 *     Hard-blocks any second add-bot in the same frame, even if it
 *     comes from a different button (per-team header + per-row in the
 *     player table) before the debounce-disabled state can propagate.
 *   - All loops over MAX_TANKS guard the slot accessor against NULL
 *     and treat NULL as "not connected" (the SP path can briefly hold
 *     a partially-initialized slot while serverSimCreateBot is
 *     allocating the ClientSim). */
static Uint32 s_addBotSentMs = 0;
static int    s_addBotExpectedConn = 0;
static int    s_addBotFrame = -1;

static int lobbyCountConnectedSlots(ClientSim *cs) {
    int n = 0;
    if (!cs) return 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        const ClientLobbySlot *lp = clientSimGetLobbySlot(cs, (BYTE)i);
        if (lp && lp->connected) n++;
    }
    return n;
}

static bool lobbyAddBotPending(ClientSim *cs) {
    if (!cs) return false;
    /* In the same ImGui frame as the last send, refuse another no matter
     * what — multiple buttons render before any one's click can update
     * the disabled state of the others. */
    if (s_addBotFrame == ImGui::GetFrameCount()) return true;
    if (s_addBotSentMs == 0) return false;
    Uint32 now = SDL_GetTicks();
    if (now - s_addBotSentMs > 2000) {
        s_addBotSentMs = 0;
        return false;
    }
    if (lobbyCountConnectedSlots(cs) >= s_addBotExpectedConn) {
        s_addBotSentMs = 0;
        return false;
    }
    return true;
}

static void lobbySendAddBotDebounced(ClientSim *cs,
                                     int namingPool, uint8_t teamNumber) {
    if (!cs) return;
    /* Drop the call entirely if anything already added a bot this
     * frame — the visible button was probably stale-clicked. */
    if (s_addBotFrame == ImGui::GetFrameCount()) return;
    /* Refuse to push past the 16-slot ceiling regardless of how the
     * server would otherwise handle it. Keeps the bot pool / subscriber
     * registry from being torched if MAX_TANKS slots are already in use
     * and a queued click slips through. */
    if (lobbyCountConnectedSlots(cs) >= MAX_TANKS) return;
    s_addBotFrame = ImGui::GetFrameCount();
    s_addBotSentMs = SDL_GetTicks();
    s_addBotExpectedConn = lobbyCountConnectedSlots(cs) + 1;
    lobbySendAddBot(cs, namingPool, teamNumber);
}

/* ── Map chooser helpers ───────────────────────────────────────────
 * The map chooser is a separate draggable ImGui window opened from the
 * lobby's Map tab. Selection triggers PACKET_LOBBY_SET_MAP in MP, or a
 * direct serverSimReloadMap call when the lobby is SP-host. State
 * persists across re-opens so the user's last tab + position are
 * remembered for the session.
 *
 * State has to be declared before the helpers that reference it.
 * s_chooseMapPrevName captures the map that was active when the
 * window opened so Cancel can restore it (no undo packet is wired
 * yet — the field is reserved for that future work). */
static bool             s_chooseMapOpen          = false;
/* Edge-trigger: SetNextWindowFocus the chooser on the frame it opens so
 * it draws above the scrim windows, but NOT every frame after — that
 * yanks focus away from anything the user clicks into the chat-hole. */
static bool             s_chooseMapFocusedOnce   = false;
/* Screen-space rect of the lobby's chat block, captured each frame so
 * the map-chooser scrim can punch a hole over it. We deliberately
 * leave the chat reachable while the chooser is open so players can
 * keep talking while picking / generating a map, but everything else
 * behind the chooser was getting accidental clicks; the scrim
 * windows below (renderChooserScrim) darken + absorb input over the
 * non-chat area. ImVec2(0,0) on both means "no chat rect yet". */
static ImVec2           s_chatBlockMin           = ImVec2(0.0f, 0.0f);
static ImVec2           s_chatBlockMax           = ImVec2(0.0f, 0.0f);
static int              s_chatRefocusFrames      = 0;
static bool             s_chatHideNav            = false;
/* Two chooser instances: one for the Server Maps tab (routes its
 * directory listing through serverSimEnumerateMapDir so it reflects
 * the server's actual map library), one for the Upload tab (always
 * uses the LOCAL filesystem so the user can browse their own files
 * before sending them up). Keeping the state separate means each tab
 * remembers its own folder / selection / search filter independently. */
static MapChooserState  s_chooseMapState         = {};
static MapChooserState  s_chooseMapUploadState   = {};
/* Third chooser instance for the Random tab — runs in randomTabOnly
 * mode so the widget renders generator controls + a live preview.
 * Config changes ripple to the server via genSeq edge detection. */
static MapChooserState  s_chooseMapRandomState   = {};
static uint32_t         s_chooseMapRandomLastSeq = 0;
/* Fourth chooser instance for the Winbolo.net Maps tab — same widget
 * the Upload / Server Maps tabs use, with a listProvider that fetches
 * folders from /api/v1/maps/{id} instead of from a local directory. */
static MapChooserState  s_chooseMapWbnState      = {};
static bool             s_chooseMapStateInited   = false;
static char             s_chooseMapPrevName[128] = "";
static int              s_chooseMapActiveTab     = 0; /* 0=server 1=upload 2=random 3=wbn */
/* Source tab the trigger/shoulder tab-cycle wants selected next frame in the
 * map chooser, or -1 for "no forced selection". Applied via
 * ImGuiTabItemFlags_SetSelected, then cleared once after the tab bar. */
static int              s_chooseMapForceTab      = -1;
/* When true, the chooser window is force-sized to almost the full
 * lobby window — leaving a few chat lines visible at the bottom.
 * Toggled by the corner icon button, or by pressing Esc while the
 * chooser window has focus. */
static bool             s_chooseMapMaximized     = false;
/* True when the user has fired at least one live-preview from a tab
 * since opening the chooser (file pick, upload kick, generate config
 * change). Drives the close-confirmation modal: dismissing the
 * window with the X (or Esc) while pending opens a "Use This Map /
 * Cancel / Keep Picking" prompt instead of silently reverting. */
static bool             s_chooseMapPreviewPending = false;
static bool             s_chooseMapWantCloseConfirm = false;
/* Cached ClientSim pointer for the chooser. Captured by
 * lobbyChooseMapOpen so the listProvider (which only gets a void*
 * ctx) can reach into the cs's lobbyMapList* state without each
 * call site rethreading the pointer. */
static ClientSim       *s_chooseMapCs             = NULL;

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
    if (!sel || !sel[0]) return;
    WB_LOG_INFO(WB_LOG_CAT_GUI,
                "[MAPPICK] server-maps onSelect sel='%s' sp=%d",
                sel, (int)clientSimIsSinglePlayer(cs));
    if (clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (sim) {
            threadsWaitForMutex();
            bool ok = serverSimReloadMap(sim, sel);
            threadsReleaseMutex();
            if (ok) s_chooseMapPreviewPending = true;
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
        s_chooseMapPreviewPending = true;
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
     * streamed reply into mapChooserSetSelectedFile (mirroring the WBN
     * tab). So return false here — no per-row thumbnail — but the
     * selected-map preview pane still fills in. */
    return false;
}

/* Main-thread pump for the Server Maps preview pane (MP only). Polls
 * the ClientSim's lobbyMapPreview* accumulator (filled async by the
 * CHANNEL_BULK preview receiver) and, once a full map's bytes have
 * arrived for the path we asked for, spills them to a worker-private
 * temp file and points the chooser at it via mapChooserSetSelectedFile.
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

    const char *tmpPath = "data/preview_cache/.sm_preview.map";
    SDL_CreateDirectory("data/preview_cache");
    FILE *fp = fopen(tmpPath, "wb");
    if (fp) {
        size_t wrote = fwrite(bytes, 1, blen, fp);
        fclose(fp);
        if (wrote == blen) {
            /* Display name = basename minus the .map suffix. */
            char disp[128];
            const char *base = SDL_strrchr(path, '/');
            base = base ? base + 1 : path;
            SDL_strlcpy(disp, base, sizeof(disp));
            size_t dl = SDL_strlen(disp);
            if (dl > 4 && SDL_strcasecmp(disp + dl - 4, ".map") == 0) {
                disp[dl - 4] = '\0';
            }
            mapChooserSetSelectedFile(&s_chooseMapState, renderer,
                                      tmpPath, disp);
        }
    }
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
    if (!picked || !picked[0]) return;
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
            s_chooseMapPreviewPending = true;
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
                s_chooseMapPreviewPending = true;
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
 * The tab browses the WBN REST catalogue. Folder listings and
 * search results are fetched on a detached std::thread; the UI
 * thread renders from a mutex-guarded parsed snapshot. Picking a
 * map sends PACKET_LOBBY_PREVIEW_WBN to the host, which downloads
 * server-side (see transport_udp_server.c's wbnDownloadWorker).
 * The preview itself rolls in over the normal MAP_CHANGE flow —
 * no client-side download or local file write here. */

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

static std::mutex                s_wbnMapsMutex;
static std::atomic<bool>         s_wbnMapsFetching{false};
static std::atomic<uint32_t>     s_wbnMapsFetchSeq{0}; /* invalidates late results */
static int                       s_wbnMapsCurrentFolderId = 0;       /* 0 = root collection */
static std::string               s_wbnMapsCurrentPath;               /* canonical friendly path of cached folder */
static std::vector<WbnMapsCrumb> s_wbnMapsCrumbs;
static std::vector<WbnMapsFolder>s_wbnMapsSubfolders;
static std::vector<WbnMapsEntry> s_wbnMapsEntries;
static std::string               s_wbnMapsError;
static bool                      s_wbnMapsHasData = false;
/* Path → folder ID. Built up as folders are fetched: each response's
 * subfolders[] gives us (childName, childId) pairs which combine
 * with the parent path to form each child's full friendly path.
 * "" maps to 0 (the root collection). */
static std::map<std::string,int> s_wbnPathToId;

/* Separate search-result cache so clearing the recursive-search
 * checkbox doesn't blow away the folder listing the user was just
 * browsing. Populated by wbnMapsParseSearchJson when the search
 * branch fires. */
static std::string                s_wbnSearchQuery;     /* last query we got results for */
static std::vector<WbnMapsEntry>  s_wbnSearchResults;
static bool                       s_wbnSearchHasData = false;
static std::atomic<bool>          s_wbnSearchFetching{false};
static std::atomic<uint32_t>      s_wbnSearchFetchSeq{0};
static std::string                s_wbnSearchError;

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
        std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
        s_wbnMapsError = langGetText(STR_DLGLOBBY_WBN_ERR_BADRESPONSE);
        s_wbnMapsHasData = false;
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
        std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
        s_wbnMapsCrumbs        = std::move(crumbs);
        s_wbnMapsSubfolders    = subs;          /* copy — also used below */
        s_wbnMapsEntries       = std::move(entries);
        s_wbnMapsCurrentFolderId = newFolderId;
        s_wbnMapsCurrentPath   = canonicalPath;
        s_wbnMapsError.clear();
        s_wbnMapsHasData       = true;

        /* Stamp every (path, id) pair we now know — current folder
         * and each subfolder — so the listProvider can resolve
         * any seen path back to a numeric ID for fetching. */
        s_wbnPathToId[canonicalPath] = newFolderId;
        for (const auto &f : subs) {
            std::string childPath = canonicalPath.empty()
                ? f.name : (canonicalPath + "/" + f.name);
            s_wbnPathToId[childPath] = f.id;
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
        std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
        s_wbnSearchError = langGetText(STR_DLGLOBBY_WBN_ERR_BADSEARCHRESPONSE);
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
             * folder). Each prefix gets stamped into s_wbnPathToId
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
     * (not just the leaf) into s_wbnPathToId — breadcrumb buttons
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
        std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
        for (const auto &p : prefixIds) {
            s_wbnPathToId[p.first] = p.second;
        }
        s_wbnSearchResults  = std::move(entries);
        s_wbnSearchQuery    = query ? query : "";
        s_wbnSearchHasData  = true;
        s_wbnSearchError.clear();
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

static std::mutex          s_spWbnMutex;
static std::atomic<bool>   s_spWbnFetching{false};
static std::atomic<uint32_t> s_spWbnFetchSeq{0};
static volatile int        s_spWbnCancel = 0; /* CURLOPT_XFERINFO sink */
static SpWbnResult         s_spWbnResult;

static void spWbnSubmit(uint32_t mapId) {
    /* Supersede any in-flight call. The cancel flag aborts curl;
     * the seq bump invalidates the completion. */
    s_spWbnCancel = 1;
    uint32_t seq = ++s_spWbnFetchSeq;
    /* Drop any undrained previous result. */
    {
        std::lock_guard<std::mutex> lk(s_spWbnMutex);
        s_spWbnResult = SpWbnResult{};
    }
    std::thread([mapId, seq]() {
        /* Wait briefly for previous thread to clear the fetching
         * flag — both threads race the same flag. */
        for (int i = 0; i < 50 && s_spWbnFetching.load(); i++) {
            SDL_Delay(10);
        }
        s_spWbnFetching.store(true);
        s_spWbnCancel = 0; /* reset for this attempt */

        SpWbnResult out;
        out.mapId = mapId;

        char infoPath[64];
        SDL_snprintf(infoPath, sizeof(infoPath), "maps/info/%u",
                     (unsigned)mapId);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] info fetch: /api/v1/%s", infoPath);
        char *infoJson = nullptr;
        int infoStatus = wbn_api_get(infoPath, &infoJson);
        if (seq != s_spWbnFetchSeq.load()) {
            free(infoJson);
            s_spWbnFetching.store(false);
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
            filePath, &bytes, &bytesLen, &s_spWbnCancel);
        WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[WBN-SP] file fetch result: /api/v1/%s -> %d (%zu bytes)",
                    filePath, dlStatus, bytesLen);

        if (seq != s_spWbnFetchSeq.load() || dlStatus == -2) {
            free(bytes);
            s_spWbnFetching.store(false);
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
            std::lock_guard<std::mutex> lk(s_spWbnMutex);
            s_spWbnResult = std::move(out);
        }
        s_spWbnFetching.store(false);
    }).detach();
}

/* Main-thread completion handler for SP picks. Drains the result
 * and applies it to the local sim. Called once per frame while
 * the WBN tab is visible. */
static void spWbnPoll(ClientSim *cs, SDL_Renderer *renderer) {
    if (!cs) return;
    SpWbnResult res;
    {
        std::lock_guard<std::mutex> lk(s_spWbnMutex);
        if (!s_spWbnResult.valid) return;
        res = std::move(s_spWbnResult);
        s_spWbnResult = SpWbnResult{};
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
            &s_chooseMapWbnState, renderer,
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
    s_chooseMapPreviewPending = true;
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
    if (s_wbnMapsFetching.exchange(true)) return; /* one at a time */
    uint32_t seq = ++s_wbnMapsFetchSeq;
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
        if (seq == s_wbnMapsFetchSeq.load()) {
            if (status == 200 && resp) {
                wbnMapsParseFolderJson(resp);
            } else {
                std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
                char err[512];
                SDL_snprintf(err, sizeof(err),
                             "Folder fetch failed (HTTP %d) for %s/api/v1/%s",
                             status,
                             (base && *base) ? base : "(no base)",
                             path);
                s_wbnMapsError = err;
            }
        }
        free(resp);
        s_wbnMapsFetching.store(false);
    }).detach();
}

static void wbnMapsKickSearchFetch(const char *queryRaw) {
    if (!queryRaw || !*queryRaw) return;
    if (s_wbnSearchFetching.exchange(true)) return;
    uint32_t seq = ++s_wbnSearchFetchSeq;
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
        if (seq == s_wbnSearchFetchSeq.load()) {
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
                std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
                char err[512];
                SDL_snprintf(err, sizeof(err),
                             "Search failed (HTTP %d) for %s/api/v1/%s",
                             status,
                             (base && *base) ? base : "(no base)",
                             path.c_str());
                s_wbnSearchError = err;
            }
        }
        free(resp);
        s_wbnSearchFetching.store(false);
    }).detach();
}

/* listProvider for the Winbolo.net Maps tab. The chooser hands us
 * relPath (the chooser's currentDir — a friendly slash-separated
 * path like "ClassicMap's Maps/classics-popular"). We:
 *   - resolve it to a numeric folder ID via s_wbnPathToId,
 *   - kick a fetch if the cache doesn't already hold that folder,
 *   - populate state->maps[] from whatever's currently cached
 *     (synchronous return; the next frame will pick up fresh data).
 *
 * Map rows are emitted with a synthetic "wbn:<id>" path which the
 * lobby's click handler parses to trigger the download/upload flow.
 * Sub-folder rows use the friendly child path so a click writes
 * that back into state->currentDir and the cycle repeats. */
static void wbnMapsListProvider(MapChooserState *state,
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
        bool srchInFlight = s_wbnSearchFetching.load();
        std::string srchErr;
        {
            std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
            query       = s_wbnSearchQuery;
            results     = s_wbnSearchResults;
            hasResults  = s_wbnSearchHasData;
            srchErr     = s_wbnSearchError;
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
        std::lock_guard<std::mutex> lk(s_wbnMapsMutex);
        auto it = s_wbnPathToId.find(relPath);
        if (it != s_wbnPathToId.end()) targetId = it->second;
        cachedId   = s_wbnMapsCurrentFolderId;
        cachedPath = s_wbnMapsCurrentPath;
        subs       = s_wbnMapsSubfolders;
        entries    = s_wbnMapsEntries;
        hasData    = s_wbnMapsHasData;
        errMsg     = s_wbnMapsError;
    }

    /* Kick a fetch if the cache doesn't match what the chooser
     * currently wants. Tracks the most recently-requested ID so
     * rapid folder clicks while one is in flight don't pile up. */
    static int s_wbnMapsLastRequested = -2;
    bool cacheMatches = hasData && (cachedPath == relPath);
    bool inFlight = s_wbnMapsFetching.load();
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
static void lobbyWbnMapsOnSelect(MapChooserState *state, void *ctx) {
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
 * provider resolves via s_wbnPathToId on the next frame. */
static void lobbyWbnMapsOnFolderJump(MapChooserState *state,
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
static bool lobbyWbnGeneratePreview(const char *entryPath,
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
static void lobbyWbnMapsTick(MapChooserState *state, SDL_Renderer *renderer,
                              void *ctx) {
    (void)state;
    ClientSim *cs = (ClientSim *)ctx;
    if (cs) spWbnPoll(cs, renderer);
}

/* Tooltip on the WBN path label = the WBN host URL so the user can
 * tell at a glance which server the catalogue is coming from. */
static void lobbyWbnMapsTooltipPrefix(MapChooserState *state, void *ctx) {
    (void)ctx;
    const char *wbnHost = httpGetBaseUrl();
    SDL_strlcpy(state->pathTooltipPrefix,
                (wbnHost && *wbnHost) ? wbnHost : "winbolo.net",
                sizeof(state->pathTooltipPrefix));
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
    switch (s_chooseMapActiveTab) {
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
    if (!s_chooseMapStateInited) {
        mapChooserInit(&s_chooseMapState, renderer);
        /* Let the chooser draw the maximize toggle overlay on the
         * preview image — it knows where the image actually lives,
         * which the surrounding lobby code doesn't. */
        s_chooseMapState.maximizePtr = &s_chooseMapMaximized;
        /* Server Maps provider: list comes from the server. */
        s_chooseMapState.provider.enumerate            = lobbyServerMapsListProvider;
        s_chooseMapState.provider.onSelect             = lobbyServerMapsOnSelect;
        s_chooseMapState.provider.onFolderJump         = lobbyServerMapsOnFolderJump;
        s_chooseMapState.provider.refreshTooltipPrefix = lobbyServerMapsTooltipPrefix;
        s_chooseMapState.provider.generatePreview      = lobbyServerMapsGeneratePreview;
        s_chooseMapState.provider.cacheScope           = "server";
        s_chooseMapState.provider.ctx                  = s_chooseMapCs;
        /* Network clients fetch the map list over the wire via
         * PACKET_LOBBY_MAP_LIST_REQ; the response lands asynchronously
         * after lobbyChooseMapEnsureInit's one-shot discoverMaps has
         * already returned an empty list. Without refreshEveryFrame
         * the chooser never re-polls the cache and shows nothing for
         * the lifetime of the dialog. The provider's own in-flight
         * gate prevents request spam. SP-host's synchronous branch
         * pays a no-op per-frame discoverMaps; the in-process scan
         * is already cheap so leave the flag on unconditionally. */
        s_chooseMapState.provider.refreshEveryFrame    = true;
        SDL_strlcpy(s_chooseMapState.crumbsRootLabel, "Maps",
                    sizeof(s_chooseMapState.crumbsRootLabel));
        /* Upload provider: local-filesystem scan via the chooser's
         * built-in helper. */
        mapChooserInit(&s_chooseMapUploadState, renderer);
        s_chooseMapUploadState.maximizePtr = &s_chooseMapMaximized;
        s_chooseMapUploadState.provider.enumerate            = mapChooserLocalFsEnumerate;
        s_chooseMapUploadState.provider.onSelect             = lobbyUploadOnSelect;
        s_chooseMapUploadState.provider.onFolderJump         = lobbyUploadOnFolderJump;
        s_chooseMapUploadState.provider.refreshTooltipPrefix = lobbyUploadTooltipPrefix;
        s_chooseMapUploadState.provider.generatePreview      = lobbyUploadGeneratePreview;
        s_chooseMapUploadState.provider.cacheScope           = "upload";
        SDL_strlcpy(s_chooseMapUploadState.crumbsRootLabel, "Maps",
                    sizeof(s_chooseMapUploadState.crumbsRootLabel));
        /* "Load from device" + "Generate Random Map" exist as dedicated
         * tabs in this window, so suppress the in-widget buttons that
         * would duplicate them. */
        s_chooseMapState.hideExtras       = true;
        s_chooseMapUploadState.hideExtras = true;
        /* Random tab — third chooser instance, runs in randomTabOnly
         * mode so the widget renders generator controls on the left
         * and the procedural preview on the right. */
        mapChooserInit(&s_chooseMapRandomState, renderer);
        s_chooseMapRandomState.maximizePtr   = &s_chooseMapMaximized;
        s_chooseMapRandomState.hideExtras    = true;
        s_chooseMapRandomState.randomTabOnly = true;
        s_chooseMapRandomLastSeq             = 0;
        /* Keep the map list narrow so the preview can claim most of
         * the row width. ~300 px fits a column of names comfortably
         * without crowding the preview. The Random tab uses a wider
         * left panel because the generator controls need more room
         * than a single column of names. */
        s_chooseMapState.leftPanelMaxW        = 300.0f;
        s_chooseMapUploadState.leftPanelMaxW  = 300.0f;
        s_chooseMapRandomState.leftPanelMaxW  = 360.0f;
        /* WBN provider — walks the WBN HTTP catalogue. refreshEveryFrame
         * because the listing lands asynchronously on a worker thread;
         * the tab needs to surface cache updates without user action. */
        mapChooserInit(&s_chooseMapWbnState, renderer);
        s_chooseMapWbnState.maximizePtr      = &s_chooseMapMaximized;
        s_chooseMapWbnState.hideExtras       = true;
        s_chooseMapWbnState.leftPanelMaxW    = 300.0f;
        s_chooseMapWbnState.provider.enumerate            = wbnMapsListProvider;
        s_chooseMapWbnState.provider.onSelect             = lobbyWbnMapsOnSelect;
        s_chooseMapWbnState.provider.onFolderJump         = lobbyWbnMapsOnFolderJump;
        s_chooseMapWbnState.provider.refreshTooltipPrefix = lobbyWbnMapsTooltipPrefix;
        s_chooseMapWbnState.provider.tick                 = lobbyWbnMapsTick;
        s_chooseMapWbnState.provider.generatePreview      = lobbyWbnGeneratePreview;
        s_chooseMapWbnState.provider.cacheScope           = "wbn";
        s_chooseMapWbnState.provider.refreshEveryFrame    = true;
        SDL_strlcpy(s_chooseMapWbnState.crumbsRootLabel, "Maps",
                    sizeof(s_chooseMapWbnState.crumbsRootLabel));
        /* Force an initial discover for each provider — mapChooserInit
         * ran discoverMaps before the providers were wired, so the
         * states landed empty. */
        s_chooseMapState.currentDir[0]       = '\0';
        s_chooseMapUploadState.currentDir[0] = '\0';
        mapChooserRefresh(&s_chooseMapState);
        mapChooserRefresh(&s_chooseMapUploadState);
        /* Land the selection on Everard if it's still entry 0. */
        if (s_chooseMapState.numMaps > 0) {
            s_chooseMapState.selectedIdx = 0;
            SDL_strlcpy(s_chooseMapState.selectedPath,
                        s_chooseMapState.maps[0].path,
                        sizeof(s_chooseMapState.selectedPath));
            SDL_strlcpy(s_chooseMapState.selectedName,
                        s_chooseMapState.maps[0].name,
                        sizeof(s_chooseMapState.selectedName));
        }
        s_chooseMapStateInited = true;
    }
}

static void lobbyChooseMapOpen(ClientSim *cs, SDL_Renderer *renderer) {
    /* Cache the cs for providers before EnsureInit so the first
     * synchronous discover sees the network ctx. */
    s_chooseMapCs = cs;
    lobbyChooseMapEnsureInit(renderer);
    /* Refresh each provider's cs ctx every time the chooser opens —
     * EnsureInit only runs once, but cs rebinds across game sessions.
     * All three providers use cs as ctx in their onSelect path. */
    s_chooseMapState.provider.ctx        = cs;
    s_chooseMapUploadState.provider.ctx  = cs;
    s_chooseMapWbnState.provider.ctx     = cs;
    /* Snapshot the currently active map so Cancel can restore it
     * once an undo packet exists. */
    const char *cur = cs ? clientSimGetMapName(cs) : "";
    SDL_strlcpy(s_chooseMapPrevName, cur ? cur : "",
                sizeof(s_chooseMapPrevName));

    /* Default the chooser's highlighted entry to whatever map is
     * currently active — so SP (which loads Everard by default) opens
     * with "Everard Island (Inbuilt)" pre-selected, and subsequent
     * opens reflect any choice made last time. Falls back to entry 0
     * (Everard) if the current map isn't in the list. */
    int matchedIdx = 0;
    if (cur && cur[0] != '\0') {
        for (int i = 0; i < s_chooseMapState.numMaps; i++) {
            if (SDL_strcasecmp(s_chooseMapState.maps[i].name, cur) == 0) {
                matchedIdx = i;
                break;
            }
        }
    }
    if (matchedIdx != s_chooseMapState.selectedIdx) {
        s_chooseMapState.selectedIdx = matchedIdx;
        SDL_strlcpy(s_chooseMapState.selectedPath,
                    s_chooseMapState.maps[matchedIdx].path,
                    sizeof(s_chooseMapState.selectedPath));
        SDL_strlcpy(s_chooseMapState.selectedName,
                    s_chooseMapState.maps[matchedIdx].name,
                    sizeof(s_chooseMapState.selectedName));
        s_chooseMapState.randomMapSelected = false;
    }

    s_chooseMapOpen = true;
    s_chooseMapPreviewPending   = false;
    s_chooseMapWantCloseConfirm = false;
}

/* Maximized chooser — separate ImGui window with its own ID so its
 * (small amount of) state lives independently of the normal one. X
 * here just un-maximizes (sets s_chooseMapMaximized = false). The
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
        s_chooseMapMaximized = false;
    }
    if (!visible) {
        ImGui::End();
        return;
    }

    /* Esc — same effect as the X. */
    if (ImGui::IsWindowFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s_chooseMapMaximized = false;
    }

    /* Mirror the visible tab — without this we'd always show the
     * server-list chooser's map even when the user was previewing
     * a generated map on the Generate tab. */
    MapChooserState *activeChooser = &s_chooseMapState;
    if (s_chooseMapActiveTab == 1)      activeChooser = &s_chooseMapUploadState;
    else if (s_chooseMapActiveTab == 2) activeChooser = &s_chooseMapRandomState;
    else if (s_chooseMapActiveTab == 3) activeChooser = &s_chooseMapWbnState;

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
            ImGui::Image((ImTextureID)tex, ImVec2(availW, availH));
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

static void lobbyChooseMapRenderWindow(ClientSim *cs, SDL_Renderer *renderer,
                                       float s, int screenW, int screenH) {
    /* Stop the preview worker on the close edge — any of the six
     * paths that flip s_chooseMapOpen to false land here on the next
     * frame, and the worker auto-restarts on the next preview request
     * if the user reopens the chooser. */
    static bool s_prevOpen = false;
    if (s_prevOpen && !s_chooseMapOpen) {
        mapChooserStopPreviewWorker();
    }
    s_prevOpen = s_chooseMapOpen;
    if (!s_chooseMapOpen) return;
    lobbyChooseMapEnsureInit(renderer);

    /* Maximized lives in its own ImGui window with a different ID so
     * its size/position are independent of the normal window's. Both
     * windows just hide/show (no destroy/recreate) — ImGui retains
     * per-window state across frames as long as nothing calls Begin
     * with the same ID, and our windowmask covers exactly one each
     * frame. */
    if (s_chooseMapMaximized) {
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

    bool open = s_chooseMapOpen;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoScrollbar
                           | ImGuiWindowFlags_NoScrollWithMouse;
    char titleBuf[64];
    SDL_snprintf(titleBuf, sizeof(titleBuf), "%s##LobbyMapChooser",
                 langGetText(STR_DLGLOBBY_CHOOSEMAP_TITLE));
    if (!ImGui::Begin(titleBuf, &open, flags)) {
        ImGui::End();
        if (!open) s_chooseMapOpen = false;
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
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    /* Tab switch detection. Clearing the new tab's preview on entry
     * stops the prior visit's stale selection from showing through. */
    static int s_lastActiveTab = -1;
    int activeTabBefore = s_chooseMapActiveTab;
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
            vis[nVis++] = 3;                                        /* WBN */
            int cur = 0;
            for (int i = 0; i < nVis; i++) {
                if (vis[i] == s_chooseMapActiveTab) { cur = i; break; }
            }
            s_chooseMapForceTab = vis[(cur + shift + nVis) % nVis];
        }
    }
    if (ImGui::BeginTabBar("##MapChooserTabs", ImGuiTabBarFlags_None)) {
        if (!inProcessServer && ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_TAB_SERVERMAPS), nullptr,
                s_chooseMapForceTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooseMapActiveTab = 0;
            if (s_lastActiveTab != 0 && activeTabBefore != 0) {
                lobbyMapTabClearSelection(&s_chooseMapState);
            }
            /* Drain any streamed MAP_PREVIEW bytes into the preview pane
             * (MP only; SP serves previews synchronously). */
            lobbyServerMapsPumpPreview(cs, renderer);
            lobbyRenderMapTab(&s_chooseMapState, renderer, s);
            ImGui::EndTabItem();
        }
        if ((inProcessServer || clientSimGetUploadPolicy(cs) != UPLOAD_POLICY_OFF) &&
            ImGui::BeginTabItem(langGetText(inProcessServer ? STR_DLGLOBBY_TAB_LOCALMAPS : STR_DLGLOBBY_TAB_UPLOAD), nullptr,
                s_chooseMapForceTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooseMapActiveTab = 1;
            if (s_lastActiveTab != 1 && activeTabBefore != 1) {
                lobbyMapTabClearSelection(&s_chooseMapUploadState);
            }
            lobbyRenderMapTab(&s_chooseMapUploadState, renderer, s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_TAB_GENERATE), nullptr,
                s_chooseMapForceTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooseMapActiveTab = 2;
            float availW = ImGui::GetContentRegionAvail().x;
            float availH = ImGui::GetContentRegionAvail().y;
            if (availH < 120.0f) availH = 120.0f;
            /* Renders generator controls on the left (where the
             * other tabs show their file list) and the procedural
             * preview on the right via the chooser's randomTabOnly
             * code path. Every config change increments genSeq —
             * we forward that to the server below so other clients
             * see the live preview. */
            mapChooserRender(&s_chooseMapRandomState, renderer,
                             availW, availH, s);
            if (cs && s_chooseMapRandomState.genSeq != s_chooseMapRandomLastSeq) {
                s_chooseMapRandomLastSeq = s_chooseMapRandomState.genSeq;
                /* selectedPath is "randommap:<seed>" — strip the
                 * prefix to get the bare seed string the server
                 * expects in PACKET_LOBBY_PREVIEW_RANDOM. */
                const char *path = s_chooseMapRandomState.selectedPath;
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
                                &s_chooseMapRandomState.genConfig);
                        threadsReleaseMutex();
                    }
                    if (ok) {
                        s_chooseMapPreviewPending = true;
                    }
                } else {
                    clientSimNetSendLobbyPreviewRandom(cs, seedStr);
                    s_chooseMapPreviewPending = true;
                }
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_TAB_WBNMAPS), nullptr,
                s_chooseMapForceTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_chooseMapActiveTab = 3;
            if (s_lastActiveTab != 3 && activeTabBefore != 3) {
                lobbyMapTabClearSelection(&s_chooseMapWbnState);
            }
            lobbyRenderMapTab(&s_chooseMapWbnState, renderer, s);

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    /* One-shot: the forced selection has been applied (or the bar wasn't
     * drawn this frame), so don't keep re-forcing it. */
    s_chooseMapForceTab = -1;
    s_lastActiveTab = s_chooseMapActiveTab;
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
                        (int)s_chooseMapPreviewPending);
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
            s_chooseMapPreviewPending = false;
            s_chooseMapOpen = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(setLbl)) {
            WB_LOG_INFO(WB_LOG_CAT_GUI,
                        "[MAPPICK] action-bar UseThisMap sp=%d previewPending=%d",
                        cs ? (int)clientSimIsSinglePlayer(cs) : -1,
                        (int)s_chooseMapPreviewPending);
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
            s_chooseMapPreviewPending = false;
            s_chooseMapOpen = false;
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
    bool wantClose = !open;
    if (!wantClose && WBUI::CancelKeyPressed()) {
        wantClose = true;
    }
    if (wantClose) {
        if (s_chooseMapPreviewPending) {
            s_chooseMapWantCloseConfirm = true;
        } else {
            s_chooseMapOpen = false;
        }
    }

    /* Confirmation popup — opened from the X-close path above and
     * also from Esc handling (same flag drives both). Three exits:
     *  - Use This Map → commit preview, close chooser.
     *  - Cancel       → revert preview to the pre-open map, close.
     *  - Keep Picking → dismiss the popup, keep the chooser open. */
    if (s_chooseMapWantCloseConfirm) {
        ImGui::OpenPopup("##MapPreviewCloseConfirm");
        s_chooseMapWantCloseConfirm = false;
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
            s_chooseMapPreviewPending = false;
            s_chooseMapOpen           = false;
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
            s_chooseMapPreviewPending = false;
            s_chooseMapOpen           = false;
            ImGui::CloseCurrentPopup();
        } else if (f == WBUI::FOOTER_CANCEL) {
            /* Re-open the chooser window — Begin's `open` flag was
             * flipped false when the user hit X, so without this
             * we'd close on the very next frame. */
            s_chooseMapOpen = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

static void lobbySendRemoveBot(ClientSim *cs, uint8_t slot) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        threadsWaitForMutex();
        serverSimRemoveBot(sim, slot);
        serverSimPublishLobbySlot(sim, slot);
        threadsReleaseMutex();
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = false;
        return;
    }
    clientSimNetSendRemoveBot(cs, slot);
}

/* Move `targetSlot` to `teamNumber`. SP-host applies via the
 * wrapper's local-transport branch; over UDP the server allows any
 * client to change its own team and gates other-target moves on
 * host / admin / openHost (matches the drag-and-drop UI gate). */
static void lobbySendTeamSet(ClientSim *cs,
                             uint8_t targetSlot, uint8_t teamNumber) {
    clientSimNetSendTeamSet(cs, targetSlot, teamNumber);
}

/* Update a bot's per-slot config (difficulty / personality / name
 * override). Pre-validates the name on the client (UX courtesy — the
 * UI can drop bad input before sending) and hands off to the wire
 * wrapper, whose local-transport branch re-runs the canonical
 * validate-and-apply path for SP-host. */
static void lobbySendBotConfig(ClientSim *cs,
                               uint8_t slot,
                               uint8_t difficulty, uint8_t personality,
                               const char *name) {
    /* Pre-send validation for the bot name.  Empty name is the
     * legitimate "leave name unchanged; difficulty/personality still
     * apply" signal — pass through.  Non-empty must clear the same
     * validator the server applies (controls, reserved leading '*',
     * mixed scripts, length), then uniqueness against the client's
     * lobby-slot mirror — bots and humans both register as connected
     * here, so this catches bot-vs-bot collisions in addition to
     * bot-vs-human.  Server is authoritative
     * (transport_udp_server.c); this is the client mirror. */
    char validated[PACKET_MAX_PLAYER_NAME];
    const char *effectiveName = name;
    if (cs && name && name[0] != '\0') {
        PlayerNameValidationError nameErr = PLAYER_NAME_OK;
        if (!playerNameValidate(name, validated,
                                sizeof(validated), &nameErr)) {
            return;
        }
        for (BYTE j = 0; j < MAX_TANKS; j++) {
            if (j == slot) continue;
            const ClientLobbySlot *other = clientSimGetLobbySlot(cs, j);
            if (!other || !other->connected) continue;
            if (playerNameCompare(other->playerName, validated) == 0) {
                return;
            }
        }
        effectiveName = validated;
    }

    clientSimNetSendLobbyBotConfig(cs, slot, difficulty, personality, effectiveName);
}

/* Change which Lua brain script a lobby bot uses. SP path mutates the
 * server sim directly; MP path goes through PACKET_LOBBY_SET_BOT_BRAIN.
 * brainIdx == 0xFF means "use the server's CLI-configured default
 * brain"; any other value indexes into the lobby brain catalogue. */
static void lobbySendSetBotBrain(ClientSim *cs,
                                 uint8_t slot, uint8_t brainIdx) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || slot >= MAX_TANKS) return;
        if (serverSimGetState(sim) != serverStateLobby) return;
        if (!serverSimGetLobbyPlayer(sim, slot)->isBot) return;
        /* serverSimSwitchBotBrain calls serverSimSetBotBrainIdxFor,
         * which publishes CTRL_LOBBY_BOT_BRAIN itself. */
        threadsWaitForMutex();
        serverSimSwitchBotBrain(sim, slot, brainIdx);
        threadsReleaseMutex();
        return;
    }
    clientSimNetSendLobbySetBotBrain(cs, slot, brainIdx);
}

/* Clear a team's metadata (color/name/pool back to defaults).
 * Mirrors PACKET_LOBBY_TEAM_CLEAR. Only called when the team is
 * empty — caller already gates on memberCount == 0. The wrapper's
 * local-transport branch handles SP-host. */
static void lobbySendTeamClear(ClientSim *cs, uint8_t teamId) {
    clientSimNetSendLobbyTeamClear(cs, teamId);
}

/* Update a team's naming pool. The team-meta write goes through the
 * wire wrapper (its local-transport branch handles SP-host). After
 * the new pool is published we re-roll every bot on the team whose
 * name wasn't manually overridden so the new pool's vibe applies
 * immediately — the SP and MP branches diverge only in which set of
 * accessors they iterate (server-sim vs client-sim mirror); both
 * land on the same wire wrapper for the per-bot rename sends. */
static void lobbySendTeamPool(ClientSim *cs,
                              uint8_t teamId, uint8_t namingPool,
                              const char *teamName) {
    if (cs && clientSimIsSinglePlayer(cs)) {
        ServerSim *sim = gameFrontGetSinglePlayerServerSim();
        if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
        /* Route the team-meta write through the wire wrapper. The
         * local-transport branch in client_net.c applies the same
         * fields (and pool-uniqueness rewrite) under the threads
         * mutex. Read the current color so the wrapper's required
         * color arg doesn't clobber it — fresh teams fall back to a
         * per-teamId default, same as the MP branch below. */
        uint8_t color = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (!clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))) {
            color = (uint8_t)((teamId - 1) & 7);
        }
        clientSimNetSendLobbyTeamMeta(cs, teamId, color, namingPool, teamName);

        /* Rename bots on this team whose names weren't overridden by
         * the host. We pick names sequentially from the new pool,
         * each call's used-list including everyone already on the
         * lobby plus the names we've assigned in this loop, so the
         * picker doesn't collide with itself. */
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (serverSimIsPlayerConnected(sim, i)) {
                usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
            }
        }
        for (int slot = 0; slot < MAX_TANKS; slot++) {
            if (!serverSimIsPlayerConnected(sim, slot)) continue;
            if (!serverSimGetLobbyPlayer(sim, slot)->isBot) continue;
            if (serverSimGetLobbyPlayer(sim, slot)->teamNumber != teamId) continue;
            if (s_botNameOverridden[slot]) continue;

            char pickBuf[32];
            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             pickBuf, sizeof(pickBuf));
            lobbySendBotConfig(cs, (uint8_t)slot,
                               clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)),
                               clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                               pickBuf);
            /* Replace this slot's entry in usedNames so subsequent
             * picks see the updated name (avoids picking the same
             * name twice within the loop). */
            for (int u = 0; u < usedCount; u++) {
                if (usedNames[u] == clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName) {
                    usedNames[u] = clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName;
                    break;
                }
            }
        }
        return;
    }
    /* MP path. */
    {
        /* If the client hasn't received the team's actual metadata yet
         * (e.g. fresh join), fall back to a sensible per-teamId default
         * color rather than the zero-initialised value, which would
         * paint every team RED until the server echoed the real one
         * back. Server is authoritative — TEAM_META_CHG will correct
         * this on the next round-trip. */
        uint8_t color = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (!clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))) {
            color = (uint8_t)((teamId - 1) & 7);
        }
        clientSimNetSendLobbyTeamMeta(cs, teamId, color, namingPool, teamName);

        /* The server can't rename existing bots when the pool changes
         * because the per-pool name table lives only on the client
         * (lobby_bot_pools.h). Fan out one BOT_CONFIG per bot on this
         * team with a name drawn from the newly-selected pool — same
         * behaviour as the SP branch above, just over the wire. */
        const char *usedNames[MAX_TANKS];
        int usedCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected &&
                clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
                usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
            }
        }
        /* Stash the assigned names locally so subsequent loop
         * iterations don't pick a name we just handed out. */
        char assigned[MAX_TANKS][32];
        for (int slot = 0; slot < MAX_TANKS; slot++) {
            if (!clientSimGetLobbySlot(cs, (BYTE)(slot))->connected) continue;
            if (!clientSimGetLobbySlot(cs, (BYTE)(slot))->isBot)       continue;
            if (clientSimGetLobbySlot(cs, (BYTE)(slot))->teamNumber != teamId) continue;
            if (slot < MAX_TANKS && s_botNameOverridden[slot]) continue;

            lobbyBotPoolPick(namingPool, usedNames, usedCount,
                             assigned[slot], sizeof(assigned[slot]));
            clientSimNetSendLobbyBotConfig(cs, (uint8_t)slot,
                clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)),
                clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                assigned[slot]);
            usedNames[usedCount++] = assigned[slot];
        }
    }
}

/* Game-settings dispatcher. settingType is one of LST_* (wire_limits.h);
 * payload is 1 or 2 bytes per server-side parser. Forwards to the
 * client_net wrapper, whose local-transport branch shares
 * serverSimApplyLobbySetting with the UDP-side packet handler so SP
 * and wire follow one code path. */
static void lobbySendSetting(ClientSim *cs,
                             uint8_t settingType,
                             const uint8_t *value, uint8_t valueLen) {
    /* Pre-send validation for setting types that have a wire-side range
     * cap on the server. Drop out-of-range values rather than letting
     * the server reject them — the dispatcher has the authoritative
     * check (server_command_dispatch.c CMD_LOBBY_SET arm) and returns
     * CMD_REJECT_INVALID. */
    if (settingType == LST_TIME_MINUTES && valueLen == 2) {
        uint16_t mins = (uint16_t)((value[0] << 8) | value[1]);
        if (mins < LOBBY_TIME_MINUTES_MIN ||
            mins > LOBBY_TIME_MINUTES_MAX) {
            mpDiagLog("[ui] lobbySendSetting REJECTED type=%d (time-minutes out of range)",
                      (int)settingType);
            return;
        }
    }
    /* Surface the click at the UI boundary so we can distinguish
     * "click never reached the wire" from "click reached the wire but
     * the server rejected/no-op'd it" from "click reached the server,
     * was applied, but the radio's checked-state isn't updating". */
    {
        uint32_t valDump = 0;
        for (uint8_t k = 0; k < valueLen && k < 4; k++) {
            valDump = (valDump << 8) | value[k];
        }
        mpDiagLog("[ui] lobbySendSetting type=%d valueLen=%d value=0x%08x",
                  (int)settingType, (int)valueLen, valDump);
    }
    clientSimNetSendLobbySetting(cs, settingType, value, valueLen);
}

/* Bounding box of interesting (non-sea) terrain in the map preview */
struct MapBounds {
    int minX, minY, maxX, maxY;
};

/* Compressed map data — stashed when map download completes so the popup
 * can decompress on demand (transportUdpClientGetMapData() is only called
 * in the one-shot preview-build block; the pointer may not remain valid). */
static BYTE        *popupCompressedData = NULL;
static int          popupCompressedLen  = 0;

/* Cached compass octant (an STR_COMPASS_* lang id, 0 = unknown) per map
 * start, indexed 1-based by startIdx. MAX_STARTS is 16, so [17] covers
 * indices 1..16. Rebuilt only when the lobby map bytes change (see
 * rebuildStartCompassCache), so the player-list column never decompresses
 * the map per frame. */
static int          s_startCompassId[MAX_STARTS + 1] = {0};

/* Cached start map-square positions (1-based, parallel to s_startCompassId)
 * plus the start bounding box and count, for the ownership-marker overlay
 * on the map previews. Rebuilt alongside the compass cache on map change so
 * the overlay never decompresses the map per frame. */
static BYTE         s_startMapX[MAX_STARTS + 1] = {0};
static BYTE         s_startMapY[MAX_STARTS + 1] = {0};
static int          s_startBboxMinX = 0, s_startBboxMinY = 0;
static int          s_startBboxMaxX = 0, s_startBboxMaxY = 0;
static BYTE         s_startCount = 0;

/* 1-based start currently hovered in a start dropdown (the combo in the
 * player list), so the inline preview can outline it. Set while a dropdown
 * entry is hovered; consumed (cleared) by the preview overlay each frame. */
static int          s_hoveredStartChoice = -1;

/* Compass octant of a start at (sx,sy) within the start bounding box
 * [minX..maxX, minY..maxY]. Map Y increases downward, so north = smaller
 * y. Returns a STR_* lang id for N/NE/E/SE/S/SW/W/NW, or C (centre) when
 * the start sits within ~1/8 of the bbox extent of the centre on both
 * axes. */
static int lobbyStartCompassStr(int sx, int sy, int minX, int minY,
                                int maxX, int maxY) {
    int cx = (minX + maxX) / 2;
    int cy = (minY + maxY) / 2;
    int dx = sx - cx;
    int dy = sy - cy;
    int tolX = (maxX - minX) / 8; if (tolX < 1) tolX = 1;
    int tolY = (maxY - minY) / 8; if (tolY < 1) tolY = 1;
    if (abs(dx) <= tolX && abs(dy) <= tolY) {
        return STR_COMPASS_C;
    }
    /* atan2 with -dy flips screen-down y back to math-up north. Result in
     * (-180,180]: 0=E, 90=N, 180=W, -90=S. Snap into 8 sectors of 45deg
     * each, biasing by half a sector so each label is centred on its
     * cardinal/intercardinal direction. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
    double deg = atan2((double)(-dy), (double)dx) * 180.0 / M_PI;
    int sector = (int)floor((deg + 22.5) / 45.0);
    sector = ((sector % 8) + 8) % 8;
    static const int kSectorStr[8] = {
        STR_COMPASS_E,  STR_COMPASS_NE, STR_COMPASS_N,  STR_COMPASS_NW,
        STR_COMPASS_W,  STR_COMPASS_SW, STR_COMPASS_S,  STR_COMPASS_SE,
    };
    return kSectorStr[sector];
}

/* Rebuild s_startCompassId from a runtime compressed map buffer. Loads a
 * transient MapPreview (the same bytes buildMapPreview consumes), computes
 * the bounding box over all starts, then fills one compass id per start.
 * Clears the cache on failure or an empty start list. */
static void rebuildStartCompassCache(const BYTE *data, int len) {
    memset(s_startCompassId, 0, sizeof(s_startCompassId));
    memset(s_startMapX, 0, sizeof(s_startMapX));
    memset(s_startMapY, 0, sizeof(s_startMapY));
    s_startCount = 0;
    s_startBboxMinX = s_startBboxMinY = 0;
    s_startBboxMaxX = s_startBboxMaxY = 0;
    MapPreview *mp = clientMapPreviewLoadFromBuffer(data, len);
    if (!mp) {
        return;
    }
    BYTE n = clientMapPreviewGetStartCount(mp);
    if (n == 0) {
        clientMapPreviewDestroy(mp);
        return;
    }
    if (n > MAX_STARTS) n = MAX_STARTS;
    int minX = 255, minY = 255, maxX = 0, maxY = 0;
    BYTE i;
    for (i = 1; i <= n; i++) {
        BYTE x, y, dir;
        if (!clientMapPreviewGetStart(mp, i, &x, &y, &dir)) continue;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    for (i = 1; i <= n; i++) {
        BYTE x, y, dir;
        if (!clientMapPreviewGetStart(mp, i, &x, &y, &dir)) continue;
        s_startMapX[i] = x;
        s_startMapY[i] = y;
        s_startCompassId[i] =
            lobbyStartCompassStr(x, y, minX, minY, maxX, maxY);
    }
    s_startCount    = n;
    s_startBboxMinX = minX;
    s_startBboxMinY = minY;
    s_startBboxMaxX = maxX;
    s_startBboxMaxY = maxY;
    clientMapPreviewDestroy(mp);
}

/* Per-start ownership codes (0-based, start index i+1) for the minimap
 * colouring: 0=unclaimed, 1=self, 2=ally, 3=enemy. Also returns an FNV-1a
 * signature so the caller can detect when a recolour rebuild is needed
 * (claims/team changes don't trigger a map re-download). Returns the count. */
static int lobbyComputeStartOwners(ClientSim *cs, int myPlayerNum,
                                   uint8_t *owners, int maxN, uint32_t *outSig) {
    int n = (int)clientSimGetLobbyStartCount(cs);
    if (n > maxN) n = maxN;
    if (n < 0)    n = 0;
    uint32_t sig = 2166136261u;
    for (int i = 1; i <= n; i++) {
        uint8_t o = (uint8_t)lobbyStartClassify(cs, lobbyStartHolderSlot(cs, i),
                                                myPlayerNum);
        owners[i - 1] = o;
        sig = (sig ^ o) * 16777619u;
    }
    sig = (sig ^ (uint32_t)n) * 16777619u;
    if (outSig) *outSig = sig;
    return n;
}

/* 1-based start whose displayed position is within radiusPx of pt (the
 * ~25px hover catch area), or -1. Shared by the overlay's hover border and
 * the mini-map drag-drop target. */
static int lobbyPreviewStartAtScreen(ImVec2 imgMin, float previewSize,
                                     int bx0, int by0, int bx1, int by1,
                                     ImVec2 pt, float radiusPx) {
    if (s_startCount == 0) return -1;
    float spanX = (float)((bx1 + 1) - bx0);
    float spanY = (float)((by1 + 1) - by0);
    if (spanX <= 0.0f || spanY <= 0.0f) return -1;
    int best = -1;
    float bestD2 = 0.0f;
    float r2 = radiusPx * radiusPx;
    for (int i = 1; i <= (int)s_startCount; i++) {
        float fx = imgMin.x + (((float)s_startMapX[i] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_startMapY[i] + 0.5f - by0) / spanY) * previewSize;
        float ex = fx - pt.x, ey = fy - pt.y, d2 = ex * ex + ey * ey;
        if (d2 > r2) continue;
        if (best < 0 || d2 < bestD2) { best = i; bestD2 = d2; }
    }
    return best;
}

/* Draw reserved-start ownership markers over the inline map preview Image.
 * Maps each cached start map-square into the displayed (cropped) image rect,
 * colours it by who claimed it (self/ally/enemy/unclaimed), and labels
 * claimed starts with the minimal unique prefix of the holder's name placed
 * toward the map edge (per the start's compass octant) so labels avoid the
 * playable centre. (imgMin, previewSize) is the on-screen Image rect; the
 * b* ints are the source-pixel crop the Image's UVs map from. */
static void drawLobbyPreviewStartOverlay(ClientSim *cs, int myPlayerNum,
                                         ImVec2 imgMin, float previewSize,
                                         int bx0, int by0, int bx1, int by1) {
    if (s_startCount == 0) return;
    float spanX = (float)((bx1 + 1) - bx0);
    float spanY = (float)((by1 + 1) - by0);
    if (spanX <= 0.0f || spanY <= 0.0f) return;

    /* Pass 1: holder slot + a compact name list for disambiguation. */
    const char *holderNames[MAX_STARTS + 1] = {0};
    int holderOf[MAX_STARTS + 1];
    int nameListIdx[MAX_STARTS + 1];
    int nHolders = 0;
    for (int i = 1; i <= (int)s_startCount; i++) {
        holderOf[i]    = lobbyStartHolderSlot(cs, i);
        nameListIdx[i] = -1;
        if (holderOf[i] >= 0) {
            const char *nm = clientSimGetLobbySlot(cs, (BYTE)holderOf[i])->playerName;
            if (nm && nm[0]) {
                nameListIdx[i] = nHolders;
                holderNames[nHolders++] = nm;
            }
        }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font   = ImGui::GetFont();
    float fsz      = ImGui::GetFontSize() * 0.85f;
    float tilePx   = (spanX > 0.0f) ? (previewSize / spanX) : 1.0f;

    for (int i = 1; i <= (int)s_startCount; i++) {
        /* Start map-square centre -> displayed image pixel. */
        float fx = imgMin.x + (((float)s_startMapX[i] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_startMapY[i] + 0.5f - by0) / spanY) * previewSize;
        if (fx < imgMin.x || fx > imgMin.x + previewSize ||
            fy < imgMin.y || fy > imgMin.y + previewSize) continue;

        /* Label: "#<n>" alone, or "#<n> <initials>" when claimed (the
         * disambiguating prefix to the right of the number). */
        char buf[40];
        if (nameListIdx[i] >= 0) {
            int plen = lobbyStartUniquePrefixLen(holderNames, nHolders, nameListIdx[i]);
            char pfx[24];
            if (plen > (int)sizeof(pfx) - 1) plen = (int)sizeof(pfx) - 1;
            memcpy(pfx, holderNames[nameListIdx[i]], (size_t)plen);
            pfx[plen] = '\0';
            SDL_snprintf(buf, sizeof(buf), "#%d %s", i, pfx);
        } else {
            SDL_snprintf(buf, sizeof(buf), "#%d", i);
        }

        /* Centre the whole field one tile toward the map edge per active
         * compass axis (e.g. a SW start -> one tile down & one tile west),
         * so the centre point is the same whether or not an initial is
         * present. */
        LobbyCompassDir dir = lobbyStartCompassDir(
            s_startMapX[i], s_startMapY[i],
            s_startBboxMinX, s_startBboxMinY, s_startBboxMaxX, s_startBboxMaxY);
        float ox, oy;
        lobbyCompassOffset(dir, &ox, &oy);
        int dirX = (ox > 0.3f) ? 1 : (ox < -0.3f ? -1 : 0);
        int dirY = (oy > 0.3f) ? 1 : (oy < -0.3f ? -1 : 0);
        float offPx = tilePx;            /* one tile, but at least 10 px */
        if (offPx < 10.0f) offPx = 10.0f;
        float cxp = fx + (float)dirX * offPx;
        float cyp = fy + (float)dirY * offPx;
        ImVec2 ts = font->CalcTextSizeA(fsz, FLT_MAX, 0.0f, buf);
        /* For a purely E/W label, anchor the near text edge at the offset
         * point so the field grows away from the start (centring would let
         * it grow back over the start); otherwise centre on the point. */
        ImVec2 tp;
        if (dirY == 0 && dirX > 0)        /* East: left edge anchored right */
            tp = ImVec2(fx + offPx, fy - ts.y * 0.5f);
        else if (dirY == 0 && dirX < 0)   /* West: right edge anchored left */
            tp = ImVec2(fx - offPx - ts.x, fy - ts.y * 0.5f);
        else                              /* N/S/diagonal/centre: centred */
            tp = ImVec2(cxp - ts.x * 0.5f, cyp - ts.y * 0.5f);
        /* Colour the whole label by ownership: you/allies a bright mint green
         * (distinct from the grass/forest greens so it stands out), enemies
         * red, unclaimed white. */
        ImU32 txtCol = IM_COL32(255, 255, 255, 255);
        if (nameListIdx[i] >= 0) {
            LobbyStartOwner o = lobbyStartClassify(cs, holderOf[i], myPlayerNum);
            if (o == LSO_ENEMY) txtCol = IM_COL32(235, 90, 90, 255);
            else                txtCol = IM_COL32(80, 255, 170, 255);
        }
        dl->AddText(font, fsz, ImVec2(tp.x + 1.0f, tp.y + 1.0f),
                    IM_COL32(0, 0, 0, 205), buf);
        dl->AddText(font, fsz, tp, txtCol, buf);
    }

    /* 1px white border around a start, marking it as "the one in focus":
     * the start under the cursor (~25px catch, also the drag-drop target)
     * and the start whose dropdown entry is currently hovered. */
    auto outlineStart = [&](int st) {
        if (st < 1 || st > (int)s_startCount) return;
        float fx = imgMin.x + (((float)s_startMapX[st] + 0.5f - bx0) / spanX) * previewSize;
        float fy = imgMin.y + (((float)s_startMapY[st] + 0.5f - by0) / spanY) * previewSize;
        float dotPx = 3.0f * previewSize / spanX;
        float half  = dotPx * 0.5f + 1.5f;
        if (half < 4.0f) half = 4.0f;
        dl->AddRect(ImVec2(fx - half, fy - half), ImVec2(fx + half, fy + half),
                    IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.0f);
    };
    outlineStart(lobbyPreviewStartAtScreen(imgMin, previewSize, bx0, by0, bx1, by1,
                                           ImGui::GetMousePos(), 25.0f));
    outlineStart(s_hoveredStartChoice);
    s_hoveredStartChoice = -1;   /* consume */
}

/* Interaction layer for the inline map preview. Called right after the map
 * Image, which stays the last item (so the caller's mapPreviewPopupOnClick
 * still works). No invisible button — that grabbed nav focus and drew a
 * light-blue focus outline. Clicking a FREE start moves you there; pressing a
 * movable claimed start and dragging reassigns its player (a manual drag).
 * Returns true if it consumed the click so the caller skips the zoom popup. */
static bool lobbyPreviewInteract(ClientSim *cs, int myPlayerNum, bool effHostMap,
                                 ImVec2 imgMin, float innerSize,
                                 int bx0, int by0, int bx1, int by1) {
    static int s_miniDragHolder = -1;   /* lobby slot being dragged, or -1 */
    bool consumed = false;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 mp = ImGui::GetMousePos();
    bool hov = ImGui::IsItemHovered();   /* the map Image (last item) */

    if (s_miniDragHolder >= 0) {
        consumed = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            s_miniDragHolder = -1;
            return consumed;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        const ClientLobbySlot *ds = clientSimGetLobbySlot(cs, (BYTE)s_miniDragHolder);
        if (ds && ds->playerName[0]) {
            ImVec2 ts = ImGui::CalcTextSize(ds->playerName);
            ImVec2 p(mp.x + 12.0f, mp.y - ts.y * 0.5f);
            dl->AddRectFilled(ImVec2(p.x - 3, p.y - 1),
                              ImVec2(p.x + ts.x + 3, p.y + ts.y + 1),
                              IM_COL32(0, 0, 0, 200), 3.0f);
            dl->AddText(p, IM_COL32(255, 255, 255, 255), ds->playerName);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            int st = lobbyPreviewStartAtScreen(imgMin, innerSize, bx0, by0, bx1, by1,
                                               mp, 25.0f);
            if (st >= 1)
                clientSimNetSendLobbyClaimStart(cs, (BYTE)s_miniDragHolder, (BYTE)st);
            s_miniDragHolder = -1;
        }
        return consumed;
    }

    if (!hov) return consumed;
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    /* On press: a free start under the cursor → claim it for yourself; a
     * movable claimed start → begin a manual drag-to-move. Both consume the
     * click so the zoom popup doesn't open. */
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        int st = lobbyPreviewStartAtScreen(imgMin, innerSize, bx0, by0, bx1, by1,
                                           mp, 25.0f);
        int holder = (st >= 1) ? lobbyStartHolderSlot(cs, st) : -1;
        if (st >= 1 && holder < 0 && myPlayerNum >= 0) {
            clientSimNetSendLobbyClaimStart(cs, (BYTE)myPlayerNum, (BYTE)st);
            consumed = true;
        } else if (holder >= 0 && (effHostMap || holder == myPlayerNum)) {
            s_miniDragHolder = holder;
            consumed = true;
        }
    }
    return consumed;
}

/* Build a 256x256 RGBA minimap from compressed map data.
 * Returns an SDL_Texture* or NULL on failure.
 * bounds is filled with the bounding box of non-sea terrain. */
static SDL_Texture *buildMapPreview(SDL_Renderer *renderer,
                                     const BYTE *compressedData, int dataLen,
                                     MapBounds *bounds,
                                     const uint8_t *startOwners, int ownerCount) {
    MinimapBounds mb;
    SDL_Texture *tex = minimapFromCompressedOwned(renderer, compressedData, dataLen,
                                                  &mb, NULL, NULL, NULL,
                                                  startOwners, ownerCount);
    if (bounds) {
        bounds->minX = mb.minX;
        bounds->minY = mb.minY;
        bounds->maxX = mb.maxX;
        bounds->maxY = mb.maxY;
    }
    return tex;
}

static const char *gameTypeStr(gameType gt) {
    switch (gt) {
        case gameOpen:             return langGetText(STR_DLGGAMEINFO_OPEN);
        case gameTournament:       return langGetText(STR_DLGGAMEINFO_TOURN);
        case gameStrictTournament: return langGetText(STR_DLGGAMEINFO_STRICT);
        default:                   return langGetText(STR_UNKNOWN);
    }
}

static const char *aiTypeStr(uint8_t ai) {
    switch (ai) {
        case 0:  return langGetText(STR_NO);
        case 1:  return langGetText(STR_YES);
        case 2:  return langGetText(STR_DLGGAMEINFO_AIADV);
        case 3:  return langGetText(STR_DLGGAMEINFO_AIFULL);
        default: return langGetText(STR_UNKNOWN);
    }
}

static void formatTimeLimit(int32_t ticks, char *buf, int bufSize) {
    if (ticks <= 0) {
        SDL_snprintf(buf, bufSize, "%s", langGetText(STR_DLGGAMEINFO_UNLIMITED));
        return;
    }
    int totalSecs = ticks / 50;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;
    if (hours > 0) {
        MessageArgs args = {};
        args.number = hours;
        SDL_snprintf(args.string1, sizeof(args.string1), "%02d", mins);
        SDL_snprintf(args.string2, sizeof(args.string2), "%02d", secs);
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_HMS, &args));
    } else if (mins > 0) {
        MessageArgs args = {};
        args.number = mins;
        SDL_snprintf(args.string1, sizeof(args.string1), "%02d", secs);
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_MS, &args));
    } else {
        MessageArgs args = {};
        args.number = secs;
        SDL_snprintf(buf, bufSize, "%s", langGetTextFmt(STR_DLGLOBBY_TIME_S, &args));
    }
}

static SDL_Texture *s_iconSuccess = nullptr;
static SDL_Texture *s_iconError   = nullptr;
static SDL_Texture *s_iconInfo    = nullptr;
static SDL_Texture *s_iconSettings = nullptr;
static SDL_Texture *s_iconBotCpuGreen  = nullptr;
static SDL_Texture *s_iconBotCpuRed    = nullptr;
static SDL_Texture *s_iconLocked       = nullptr;
static bool         s_iconsAttempted = false;
/* The renderer instance the icons above were created against. SDL_Texture
 * is tied to the renderer that created it, so if the renderer instance
 * pointer changes between calls (e.g. across a game→lobby transition
 * that recreates the renderer) the cached textures reference dead GPU
 * resources. Track it and reload on mismatch — same pattern as
 * imgui_mapchooser's loadViewModeIconsOnce. */
static SDL_Renderer *s_iconsRenderer = nullptr;

/* Tank sprite used as the team identity badge in the lobby header.
 * Loaded once on first lobby render; tinted with the team color via
 * a darkened semi-transparent overlay. */
static SDL_Texture *s_tankSelf04 = nullptr;
static SDL_Texture *s_tankEvil04 = nullptr;
static SDL_Texture *s_tankGood04 = nullptr;
static bool         s_tankSelf04Attempted = false;

/* stb_image entry points — defined in C, declared with C linkage
 * so the C++ linker finds them. Mirror of how imgui_welcome.cpp
 * pulls them in (its include of stb_image.h provides the same). */
extern "C" {
    unsigned char *stbi_load_from_memory(const unsigned char *, int,
                                         int *, int *, int *, int);
    void stbi_image_free(void *);
}

/* Minimal PNG loader (mirror of imgui_welcome.cpp's loadPng) — uses
 * SDL_IOFromFile + stb_image so it works on Android APK assets and
 * regular filesystem alike. */
static SDL_Texture *loadLobbyPng(SDL_Renderer *renderer, const char *filename) {
    SDL_IOStream *io = SDL_IOFromFile(filename, "rb");
    if (!io) {
        char path[512];
        SDL_snprintf(path, sizeof(path), "data/%s", filename);
        io = SDL_IOFromFile(path, "rb");
    }
    if (!io) return nullptr;
    Sint64 size = SDL_GetIOSize(io);
    if (size <= 0) { SDL_CloseIO(io); return nullptr; }
    unsigned char *buf = (unsigned char *)SDL_malloc((size_t)size);
    if (!buf) { SDL_CloseIO(io); return nullptr; }
    SDL_ReadIO(io, buf, (size_t)size);
    SDL_CloseIO(io);
    int w, h, ch;
    unsigned char *data = stbi_load_from_memory(buf, (int)size, &w, &h, &ch, 4);
    SDL_free(buf);
    if (!data) return nullptr;
    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, data, w * 4);
    if (!surf) { stbi_image_free(data); return nullptr; }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_DestroySurface(surf);
    stbi_image_free(data);
    return tex;
}

static SDL_Texture *getTankSelf04Texture(SDL_Renderer *renderer) {
    if (s_tankSelf04Attempted) return s_tankSelf04;
    s_tankSelf04Attempted = true;
    s_tankSelf04 = loadLobbyPng(renderer, "svg/tank_self_04.png");
    if (s_tankSelf04) {
        SDL_SetTextureScaleMode(s_tankSelf04, SDL_SCALEMODE_LINEAR);
    }
    return s_tankSelf04;
}

/* Red enemy tank — used next to player rows on teams different from
 * the local player's. Loaded lazily on first use, same pattern as
 * getTankSelf04Texture. */
static SDL_Texture *getTankEvil04Texture(SDL_Renderer *renderer) {
    static bool attempted = false;
    if (attempted) return s_tankEvil04;
    attempted = true;
    s_tankEvil04 = loadLobbyPng(renderer, "svg/tank_evil_04.png");
    if (s_tankEvil04) {
        SDL_SetTextureScaleMode(s_tankEvil04, SDL_SCALEMODE_LINEAR);
    }
    return s_tankEvil04;
}

/* "Good" ally tank (yellow tone) — used to distinguish the local
 * player's own row from the rest of their team. Falls back to
 * tank_self_04 if the asset isn't there. */
static SDL_Texture *getTankGood04Texture(SDL_Renderer *renderer) {
    static bool attempted = false;
    if (attempted) return s_tankGood04;
    attempted = true;
    s_tankGood04 = loadLobbyPng(renderer, "svg/tank_good_04.png");
    if (s_tankGood04) {
        SDL_SetTextureScaleMode(s_tankGood04, SDL_SCALEMODE_LINEAR);
    }
    return s_tankGood04;
}

static void loadStatusIconsOnce(SDL_Renderer *renderer, float scale) {
    /* If we've loaded against this exact renderer already, nothing
     * to do. If the renderer pointer differs (game→lobby may have
     * recreated it; SDL3 textures don't survive that), destroy the
     * stale textures and reload. */
    if (s_iconsAttempted && s_iconsRenderer == renderer) return;
    if (s_iconsAttempted && s_iconsRenderer != renderer) {
        if (s_iconSuccess)     { SDL_DestroyTexture(s_iconSuccess);     s_iconSuccess     = nullptr; }
        if (s_iconError)       { SDL_DestroyTexture(s_iconError);       s_iconError       = nullptr; }
        if (s_iconInfo)        { SDL_DestroyTexture(s_iconInfo);        s_iconInfo        = nullptr; }
        if (s_iconSettings)    { SDL_DestroyTexture(s_iconSettings);    s_iconSettings    = nullptr; }
        if (s_iconBotCpuGreen) { SDL_DestroyTexture(s_iconBotCpuGreen); s_iconBotCpuGreen = nullptr; }
        if (s_iconBotCpuRed)   { SDL_DestroyTexture(s_iconBotCpuRed);   s_iconBotCpuRed   = nullptr; }
        if (s_iconLocked)      { SDL_DestroyTexture(s_iconLocked);      s_iconLocked      = nullptr; }
    }
    s_iconsAttempted = true;
    s_iconsRenderer  = renderer;

    int iconPx = (int)(18.0f * scale);
    if (iconPx < 16) iconPx = 16;

    struct {
        SDL_Texture **target;
        const char   *relPath;
    } icons[] = {
        { &s_iconSuccess,  "data/ui/dialog-success.svg" },
        { &s_iconError,    "data/ui/dialog-error.svg" },
        { &s_iconInfo,     "data/ui/dialog-info.svg" },
        { &s_iconSettings, "data/ui/settings.svg" },
        { &s_iconBotCpuGreen, "data/ui/bot-cpu-green.svg" },
        { &s_iconBotCpuRed,   "data/ui/bot-cpu-red.svg" },
    };

    for (int i = 0; i < (int)(sizeof(icons) / sizeof(icons[0])); i++) {
        *icons[i].target = imguiLoadSvgIcon(renderer, icons[i].relPath, iconPx);
        if (*icons[i].target == nullptr) {
            char basePathBuf[FILENAME_MAX];
            const char *base = SDL_GetBasePath();
            if (base) {
                SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                             "%s%s", base, icons[i].relPath);
                *icons[i].target = imguiLoadSvgIcon(renderer, basePathBuf, iconPx);
            }
        }
    }

    /* Lock badge — rasterised as a white alpha mask so the lockBadge
     * theme color tints it at draw time (matches the previous orange
     * "[locked]" pill). */
    s_iconLocked = imguiLoadSvgIconWhite(renderer, "data/ui/mapeditor/locked.svg", iconPx);
    if (s_iconLocked == nullptr) {
        char basePathBuf[FILENAME_MAX];
        const char *base = SDL_GetBasePath();
        if (base) {
            SDL_snprintf(basePathBuf, sizeof(basePathBuf),
                         "%sdata/ui/mapeditor/locked.svg", base);
            s_iconLocked = imguiLoadSvgIconWhite(renderer, basePathBuf, iconPx);
        }
    }
}

/* Currently-expanded bot slot for the AiConfig sub-row, or -1. */
static int s_expandedBotSlot = -1;

/* Tab the trigger/shoulder tab-cycle wants selected next frame in the
 * tabbed lobby layout, or -1 for "no forced selection". Set from the
 * LT/RT (or native L1/R1) shift, applied via ImGuiTabItemFlags_SetSelected,
 * then cleared after the tab bar. */
static int s_lobbyForceTab = -1;

/* Kick-confirm dialog state. Populated when an authorised player picks
 * "Kick" from a row's right-click context menu; the modal at the bottom
 * of renderTeamGroupedPlayers reads it on the next frame. */
static int  s_kickPendingSlot = -1;
static char s_kickPendingName[64] = {0};
static bool s_kickPendingOpen = false;
static int  s_makeHostPendingSlot = -1;
static char s_makeHostPendingName[64] = {0};
static bool s_makeHostPendingOpen = false;

/* Forward decl — defined below the team renderer. */
static void renderBotAiConfig(ClientSim *cs,
                              int slot, int teamId, float s);

/* ── Layout A — team-grouped player list ──────────────────────────
 * Renders players grouped under team headers with color tints from
 * WbTheme. Replaces the flat 5-column table with the mockup's
 * "team containers" model. Sized to fit inside the calling child
 * window. Returns nothing — purely UI. */
static void renderLockBadge(void);

/* Ranked-game eligibility shape: exactly two teams with equal sizes
 * of 1/2/3 connected humans (1v1, 2v2, 3v3). Used by the Ranked-game
 * checkbox tooltip AND by the Ready button (Ready is disabled when
 * the lobby is flagged Ranked but the current shape doesn't qualify,
 * so the host can keep Ranked on for the games-list filter even
 * while shuffling players around). Returns the breakdown so callers
 * can show the same tooltip text. */
struct RankedEligibility {
    bool sizesEligible;
    int  teamsInUse;
    int  firstSize;
    int  secondSize;
};
static RankedEligibility computeRankedEligibility(ClientSim *cs) {
    RankedEligibility r = {false, 0, 0, 0};
    int teamSizes[17] = {0};
    for (int i = 0; i < MAX_TANKS; i++) {
        const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
        if (!ls || !ls->connected || ls->isBot) continue;
        uint8_t t = ls->teamNumber;
        if (t == 0 || t > 16) continue;
        if (teamSizes[t] == 0) r.teamsInUse++;
        teamSizes[t]++;
    }
    for (int t = 1; t <= 16; t++) {
        if (teamSizes[t] == 0) continue;
        if (r.firstSize == 0) r.firstSize = teamSizes[t];
        else                  r.secondSize = teamSizes[t];
    }
    r.sizesEligible = (r.teamsInUse == 2) &&
                      (r.firstSize == r.secondSize) &&
                      (r.firstSize >= 1 && r.firstSize <= 3);
    return r;
}
static void rankedShapeTooltip(const RankedEligibility &r) {
    MessageArgs args = {};
    args.number  = r.teamsInUse;
    SDL_strlcpy(args.string1,
                langGetText(r.teamsInUse == 1 ? STR_DLGLOBBY_RANKED_SHAPE_TEAM
                                               : STR_DLGLOBBY_RANKED_SHAPE_TEAMS),
                sizeof(args.string1));
    args.number2 = r.firstSize;
    args.number3 = r.secondSize;
    ImGui::SetTooltip("%s",
        langGetTextFmt(STR_DLGLOBBY_RANKED_SHAPE_TIP, &args));
}

/* True when myPlayerNum holds the lobby host role. Keeps the
 * host-identity test uniform across the lobby UI. */
static bool isLobbyHost(ClientSim *cs, int myPlayerNum) {
    return myPlayerNum >= 0 && myPlayerNum == clientSimGetLobbyHostSlot(cs);
}

/* Compact "Allow New Players:  [ ] Now   [ ] During game" row. Host
 * only and multiplayer only (single-player has no UDP listener). Used
 * to live inside renderTeamGroupedPlayers; hoisted to the parent so
 * the PlayerPanel and MapPanel top edges stay aligned. */
static void renderAllowNewPlayersRow(ClientSim *cs,
                                     int myPlayerNum, float s) {
    bool isHost = isLobbyHost(cs, myPlayerNum);
    bool isLocalAdmin = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                        (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                         & PLAYER_FLAG_ADMIN));
    bool effectiveHost = isHost || clientSimGetLobbyOpenHost(cs) || isLocalAdmin;
    /* Diagnostic: log entry state on every call, throttled to changes
     * only. Fires BEFORE the early-return so we can see whether the
     * function is reached at all and which condition trips the
     * early-out. */
    /* Row stays visible to non-host players too so they can see the
     * Ranked Game indicator — but the "Allow New Players" controls
     * and the Ranked toggle itself stay disabled for them. */
    if (!clientSimHasTransport(cs) || clientSimIsSinglePlayer(cs)) return;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Allow New Players:");
    if (!effectiveHost) ImGui::BeginDisabled();
    bool allowJoin = clientSimGetLobbyAllowNewPlayers(cs);
    ImGui::SameLine();
    char allowNowId[64];
    SDL_snprintf(allowNowId, sizeof(allowNowId), "%s##allowNow", langGetText(STR_DLGLOBBY_ALLOW_NOW));
    if (ImGui::Checkbox(allowNowId, &allowJoin)) {
        clientSimNetSendLockToggle(cs, allowJoin);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_ALLOWNOW));
    }
    bool autoLockLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_AUTO_LOCK_ON_GAME) != 0;
    bool rankedForcesAutoLock = clientSimGetLobbyRanked(cs);
    bool duringGame = !clientSimGetLobbyAutoLockOnGameStart(cs);
    if (rankedForcesAutoLock) duringGame = false;
    bool autoLockDisabled = autoLockLocked || rankedForcesAutoLock;
    if (autoLockDisabled) ImGui::BeginDisabled();
    ImGui::SameLine();
    char allowDuringId[64];
    SDL_snprintf(allowDuringId, sizeof(allowDuringId), "%s##allowDuring", langGetText(STR_DLGLOBBY_ALLOW_DURING));
    if (ImGui::Checkbox(allowDuringId, &duringGame)) {
        uint8_t v = duringGame ? 0 : 1;  /* invert */
        lobbySendSetting(cs, LST_AUTO_LOCK_ON_GAME, &v, 1);
    }
    if (autoLockDisabled) ImGui::EndDisabled();
    if (autoLockLocked) {
        ImGui::SameLine(0.0f, 4.0f * s);
        renderLockBadge();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (rankedForcesAutoLock) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_AUTOLOCK));
        } else if (!autoLockLocked) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_ALLOWDURING));
        }
    }
    if (!effectiveHost) ImGui::EndDisabled();

    /* "Ranked game" — sits to the right of the Allow-New-Players
     * controls so it's prominent on the lobby's top row. Visible to
     * every player so newcomers can see whether they're walking into
     * a ranked match; only host / admin / openHost-empowered players
     * can toggle. Server enforces the actual bots-off / no-Open-type
     * constraints regardless of who tries to flip the related
     * controls. Toggling the flag also clears everyone's ready bit
     * via the existing auto-unready broadcast path.
     *
     * Ranked is a winbolo.net feature (results are reported to WBN
     * for ladder ranking and the tracker advertises the flag), so
     * the toggle is hidden entirely in LAN / direct-IP / single-
     * player games AND in Internet games where the host process
     * isn't signed in to WBN. Non-host clients learn the host's
     * WBN-availability via lobbyWbnAvailable in CTRL_LOBBY_SETTINGS. */
    if (!clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs) &&
        clientSimGetLobbyWbnAvailable(cs)) {
        ImGui::SameLine(0.0f, 16.0f * s);
        bool rankedV = clientSimGetLobbyRanked(cs);

        /* Ranked toggle policy:
         *   - Host-only.
         *   - Blocked while bots are present so the host explicitly
         *     removes them first (no silent kick on flip-on).
         *   - Always allow turning Ranked OFF, even if the shape
         *     drifted while it was on.
         *   - Shape (1v1 / 2v2 / 3v3) is NOT a gate here — the host
         *     may want the lobby flagged Ranked so it shows up under
         *     that filter in the games list while players join.
         *     Eligibility is enforced at start time by disabling the
         *     Ready button (server silently drops bad Ready requests). */
        int botCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
            if (ls && ls->connected && ls->isBot) botCount++;
        }
        bool botsBlock = (botCount > 0) && !rankedV;
        RankedEligibility re = computeRankedEligibility(cs);

        bool rankedLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_RANKED) != 0;
        bool canToggle = effectiveHost && !botsBlock && !rankedLocked;
        if (!canToggle) ImGui::BeginDisabled();
        char rankedId[64];
        SDL_snprintf(rankedId, sizeof(rankedId), "%s##ranked", langGetText(STR_DLGLOBBY_RANKED));
        if (ImGui::Checkbox(rankedId, &rankedV)) {
            uint8_t v = rankedV ? 1 : 0;
            lobbySendSetting(cs, LST_RANKED, &v, 1);
        }
        if (!canToggle) ImGui::EndDisabled();
        if (rankedLocked) renderLockBadge();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (rankedLocked) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
            } else if (!effectiveHost) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_NOTHOST));
            } else if (botsBlock) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_BOTS));
            } else if (rankedV && !re.sizesEligible) {
                rankedShapeTooltip(re);
            } else {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_INFO));
            }
        }
    }

    /* "Balance from WBN" — sits to the right of the Ranked checkbox.
     * Host-only. Opens a confirm popup explaining that teams will be
     * cleared and replaced with two skill-balanced teams; if bots are
     * present the popup offers a third button to kick them so the
     * matchup is pure-human. After confirm, the existing
     * BALANCE_REQUEST -> BALANCE_PROPOSAL -> Apply/Dismiss flow runs
     * normally (proposal shows on every client; host applies or
     * dismisses). */
    {
        int connectedCount = 0;
        int botCount = 0;
        for (int i = 0; i < MAX_TANKS; i++) {
            const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)i);
            if (!ls || !ls->connected) continue;
            connectedCount++;
            if (ls->isBot) botCount++;
        }
        bool proposalActive = clientSimIsBalanceProposalActive(cs);
        bool enoughForBalance = (connectedCount >= 2);
        bool canBalance = effectiveHost && enoughForBalance && !proposalActive;
        char kBalancePopup[64];
        SDL_snprintf(kBalancePopup, sizeof(kBalancePopup), "%s##balwbn",
                     langGetText(STR_DLGLOBBY_BAL_POPUP_TITLE));

        /* Status feedback for the most recent Balance request — read
         * after the popup confirm sets s_balReqSentMs, displayed in
         * a small label next to the outer Balance button. */
        static uint64_t   s_balReqSentMs         = 0;
        static const char *s_balLastResultText   = NULL;
        static ImVec4     s_balLastResultColor   = ImVec4(1, 1, 1, 1);
        static uint64_t   s_balLastResultUntilMs = 0;

        /* Balance-from-WBN visibility:
         *   - Hidden in SP / LAN-only (WBN never runs there).
         *   - Hidden when the host process isn't signed in to WBN
         *     (server's wbnRunning guard would drop every click).
         *   - Hidden for non-host / non-admin clients — only the
         *     player who can act on the proposal needs to see the
         *     request affordance. effectiveHost covers host + admin
         *     + openHost-empowered slots. */
        if (effectiveHost &&
            !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs) &&
            clientSimGetLobbyWbnAvailable(cs)) {
        ImGui::SameLine(0.0f, 12.0f * s);
        if (!canBalance) ImGui::BeginDisabled();
        char balBtnLabel[64];
        SDL_snprintf(balBtnLabel, sizeof(balBtnLabel), "%s##balwbn_btn",
                     langGetText(STR_DLGLOBBY_BAL_BTN));
        bool outerClicked = ImGui::Button(balBtnLabel);
        if (!canBalance) ImGui::EndDisabled();

        /* Short status to the right of the button so the host gets
         * feedback on the last Balance request. s_balReqSentMs is
         * set when the popup's confirm button fires; the live
         * "Asking WBN…" message shows for up to 8 s while we wait.
         * On a successful proposal we flip to "Proposal ready" for
         * 6 s; on timeout we surface "No response from WBN" for 8 s
         * so the host knows the request didn't land. */
        {
            uint64_t now = SDL_GetTicks();
            const char *liveText = NULL;
            ImVec4 liveColor;
            if (s_balReqSentMs != 0) {
                uint64_t arrivedMs =
                    clientSimGetLastBalanceProposalArrivedMs(cs);
                uint64_t failedMs =
                    clientSimGetLastBalanceFailedMs(cs);
                if (arrivedMs != 0 && arrivedMs >= s_balReqSentMs) {
                    /* Server auto-applies the split now, so we use the
                     * one-shot timestamp the dispatcher latches when the
                     * proposal event arrives — proposalActive itself is
                     * cleared on the same mutex hold, so reading it
                     * here would always miss the success transition. */
                    s_balLastResultText  = langGetText(STR_DLGLOBBY_BAL_STATUS_BALANCED);
                    s_balLastResultColor = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
                    s_balLastResultUntilMs = now + 6000;
                    s_balReqSentMs = 0;
                } else if (failedMs != 0 && failedMs >= s_balReqSentMs) {
                    /* Server told us the WBN call returned without a
                     * usable proposal — flip to the failure pill now
                     * instead of waiting out the 8 s NOREPLY clock. */
                    s_balLastResultText  = langGetText(STR_DLGLOBBY_BAL_STATUS_FAILED);
                    s_balLastResultColor = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
                    s_balLastResultUntilMs = now + 8000;
                    s_balReqSentMs = 0;
                } else if (now - s_balReqSentMs < 8000) {
                    liveText  = langGetText(STR_DLGLOBBY_BAL_STATUS_ASKING);
                    liveColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                } else {
                    s_balLastResultText  = langGetText(STR_DLGLOBBY_BAL_STATUS_NOREPLY);
                    s_balLastResultColor = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
                    s_balLastResultUntilMs = now + 8000;
                    s_balReqSentMs = 0;
                }
            }
            const char *showText = liveText ? liveText :
                (s_balLastResultText && now < s_balLastResultUntilMs)
                    ? s_balLastResultText : NULL;
            ImVec4 showColor = liveText ? liveColor : s_balLastResultColor;
            if (showText) {
                ImGui::SameLine(0.0f, 8.0f * s);
                ImGui::TextColored(showColor, "%s", showText);
            }
        }
        if (outerClicked) {
            ImGui::OpenPopup(kBalancePopup);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (!effectiveHost) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BAL_TOOLTIP_NOTHOST));
            } else if (!enoughForBalance) {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BAL_TOOLTIP_NOTENOUGH));
            } else {
                ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BAL_TOOLTIP_INFO));
            }
        }

        /* The confirm popup body. ImGui::BeginPopupModal is the modal
         * variant — it dims everything underneath and traps focus
         * until the user picks a button. */
        ImGui::SetNextWindowSize(ImVec2(420.0f * s, 0.0f), ImGuiCond_Appearing);
        static bool s_balOpen = true; s_balOpen = true;
        if (ImGui::BeginPopupModal(kBalancePopup, &s_balOpen,
                                    ImGuiWindowFlags_AlwaysAutoResize
                                    | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("%s", langGetText(STR_DLGLOBBY_BAL_POPUP_BODY));
            if (botCount > 0) {
                ImGui::Spacing();
                if (botCount == 1) {
                    ImGui::TextWrapped("%s", langGetText(STR_DLGLOBBY_BAL_POPUP_BOT_SINGULAR));
                } else {
                    MessageArgs args = {};
                    args.number = botCount;
                    ImGui::TextWrapped("%s", langGetTextFmt(STR_DLGLOBBY_BAL_POPUP_BOT_PLURAL, &args));
                }
            }
            ImGui::Spacing();

            float btnW = 140.0f * s;
            float btnH = 0.0f;
            WBUI::PushCancelStyle();
            bool balCancel = ImGui::Button("Cancel##balcancel", ImVec2(btnW, btnH));
            WBUI::PopCancelStyle();
            if (balCancel || WBUI::CancelKeyPressed()) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            uint8_t teamSize = (uint8_t)(connectedCount / 2);
            if (teamSize == 0) teamSize = 1;
            if (botCount > 0) {
                char balBotsLabel[64], balHumanLabel[64];
                SDL_snprintf(balBotsLabel,  sizeof(balBotsLabel),  "%s##balbots",
                             langGetText(STR_DLGLOBBY_BAL_BOTS_INCLUDED));
                SDL_snprintf(balHumanLabel, sizeof(balHumanLabel), "%s##balhuman",
                             langGetText(STR_DLGLOBBY_BAL_HUMANS_ONLY));
                if (ImGui::Button(balBotsLabel, ImVec2(btnW, btnH))) {
                    s_balReqSentMs = SDL_GetTicks();
                    s_balLastResultText = NULL;
                    clientSimNetSendBalanceRequest(cs, teamSize, /*includeBots=*/true);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(balHumanLabel, ImVec2(btnW, btnH))) {
                    s_balReqSentMs = SDL_GetTicks();
                    s_balLastResultText = NULL;
                    clientSimNetSendBalanceRequest(cs, teamSize, /*includeBots=*/false);
                    ImGui::CloseCurrentPopup();
                }
            } else {
                char balGoLabel[64];
                SDL_snprintf(balGoLabel, sizeof(balGoLabel), "%s##balgo",
                             langGetText(STR_DLGLOBBY_BAL_GO));
                if (ImGui::Button(balGoLabel, ImVec2(btnW, btnH))) {
                    s_balReqSentMs = SDL_GetTicks();
                    s_balLastResultText = NULL;
                    clientSimNetSendBalanceRequest(cs, teamSize, /*includeBots=*/false);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        } /* end if (winbolonetIsRunning()) — Balance-from-WBN gating */
    }
}

/* Copy src into out, truncating with a trailing "..." if it's wider than
 * maxW pixels. Keeps long player names from overflowing the name column and
 * pushing the start dropdown into the Ready button on small windows. */
static void lobbyTruncateName(const char *src, float maxW, char *out, size_t outSz) {
    if (outSz == 0) return;
    if (!src) { out[0] = '\0'; return; }
    if (maxW <= 1.0f || ImGui::CalcTextSize(src).x <= maxW) {
        SDL_strlcpy(out, src, outSz);
        return;
    }
    float budget = maxW - ImGui::CalcTextSize("...").x;
    int len = (int)SDL_strlen(src);
    int n = 0;
    float w = 0.0f;
    while (n < len) {
        float cw = ImGui::CalcTextSize(src + n, src + n + 1).x;
        if (w + cw > budget) break;
        w += cw;
        n++;
    }
    if (n > (int)outSz - 4) n = (int)outSz - 4;
    if (n < 0) n = 0;
    memcpy(out, src, (size_t)n);
    out[n] = '\0';
    SDL_strlcat(out, "...", outSz);
}

static void renderTeamGroupedPlayers(ClientSim *cs,
                                     int myPlayerNum, float s, bool isHost) {
    /* Lazy-load the badge / bot-cpu icons. Used to be done inside
     * renderConnectivityBadge, but we now skip that in SP / LAN-only
     * mode where the badge has nothing to report — the bot-cpu PNGs
     * still need to come up though, so trigger it here too. The
     * helper is idempotent (s_iconsAttempted guard). */
    {
        SDL_Renderer *r = sdl3DrawGetRenderer();
        if (r) loadStatusIconsOnce(r, s);
    }

    /* Helper to count members per team for header strings. */
    int memberCount[16] = {0};
    int botCount[16]    = {0};
    int unassignedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
        uint8_t t = clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber;
        if (t == 0) {
            unassignedCount++;
        } else if (t < 16) {
            memberCount[t]++;
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->isBot) botCount[t]++;
        }
    }

    /* Walk teams 1..15, render those with members. Then unassigned. */
    bool isLocalAdmin = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                         (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                          & PLAYER_FLAG_ADMIN));
    bool effectiveHost = isHost || clientSimGetLobbyOpenHost(cs) || isLocalAdmin;

    /* "Allow new players" row is rendered by the caller above the panels
     * so the PlayerPanel and MapPanel top edges stay aligned in Y.
     * "+ Add Team" lives in the footer below the teams list (see
     * the bottom of this function). */
    (void)effectiveHost;  /* still used by the footer "Add Team" below */

    /* Start-picker column alignment. The per-row start dropdown is
     * centered in the horizontal gap between the right edge of the
     * widest player name(+HOST/ADMIN/BOT tag) across every row and the
     * left edge of the Ready/Not-ready cell, so all the dropdowns line
     * up in one column for everyone. Measured this frame, applied the
     * next (a one-frame lag is invisible for a near-static lobby). */
    static float s_startComboCenterX = 0.0f;
    static float s_startComboReadyLeft = 0.0f;  /* Ready cell left edge (last frame) */
    const float  appliedStartCenterX = s_startComboCenterX;
    const float  appliedReadyLeft    = s_startComboReadyLeft;
    float startColNameMaxRight = 0.0f;  /* widest name(+tag) right edge */
    float startColPingRightX   = 0.0f;  /* ping/gear column right edge */
    float startColReadyLeftX   = 0.0f;  /* Ready cell left edge */

    for (int teamId = 1; teamId < 16; teamId++) {
        /* Teams 1 and 2 are always rendered (the lobby's two default
         * sides) — the host always has somewhere to drop the first
         * bot. Teams 3..15 render when they have members OR when the
         * host has explicitly added them via "+ Add Team"
         * (lobbyTeamInUse=1). Without the in_use check, freshly added
         * teams would vanish on the same frame because they have no
         * members yet. */
        bool persistTeam = (teamId == 1 || teamId == 2)
                        || clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId));
        if (!persistTeam && memberCount[teamId] == 0) continue;

        /* Team color from theme; falls back to gray for un-themed teams. */
        ImU32 tc;
        uint8_t colorIdx = clientSimGetLobbyTeamColor(cs, (BYTE)(teamId));
        if (clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId)) && colorIdx < 8) {
            tc = g_theme->teamColors[colorIdx];
        } else {
            /* Default per-team color: cycle through palette by teamId. */
            tc = g_theme->teamColors[(teamId - 1) & 7];
        }

        /* Team child window — uses the default ChildBg from the theme.
         * Team color is reserved for the header strip only (see below)
         * so the body stays neutral and the team identity reads as a
         * banner rather than a flood fill. */
        char teamFrame[32];
        SDL_snprintf(teamFrame, sizeof(teamFrame), "##team%d", teamId);
        ImGui::BeginChild(teamFrame,
                          ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

        /* Color header strip — fills the header row's full width with
         * the team color at low alpha, drawn under the header widgets
         * via the window draw list. Renders before the swatch so the
         * widgets sit on top of it. */
        ImVec2 stripStart = ImGui::GetCursorScreenPos();
        float  stripH     = ImGui::GetTextLineHeightWithSpacing() + 4.0f * s;
        float  contentW   = ImGui::GetContentRegionAvail().x;
        ImU32  stripCol   = (tc & 0x00FFFFFF) | (0x28 << 24);
        ImGui::GetWindowDrawList()->AddRectFilled(
            stripStart,
            ImVec2(stripStart.x + contentW, stripStart.y + stripH),
            stripCol, 2.0f);

        /* Team identity badge moved off the team header; each player
         * row now renders a green (self/ally) or red (enemy) tank
         * icon to the left of its name, so team affiliation reads
         * per-row instead of just in the header.  Pad a few pixels
         * before the "Team N" label so it doesn't hug the left edge. */
        ImGui::Dummy(ImVec2(6.0f * s, 0));
        ImGui::SameLine(0.0f, 0.0f);

        /* Team name — always "Team N", non-editable. Renaming was
         * dropped per UX feedback; the number is enough identity
         * alongside the colored tank badge.  AlignTextToFramePadding
         * vertically centers both text spans with the frame-padded
         * widgets on the same row (matches the "Bot Naming:" label). */
        char defaultName[16];
        {
            MessageArgs args = {};
            args.number = teamId;
            SDL_snprintf(defaultName, sizeof(defaultName), "%s", langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args));
        }
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(tc));
        ImGui::Text("%s", defaultName);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        {
            char membersStr[64];
            if (memberCount[teamId] == 1) {
                SDL_snprintf(membersStr, sizeof(membersStr), "%s",
                             langGetText(STR_DLGLOBBY_TEAM_MEMBERS_1));
            } else {
                MessageArgs args = {};
                args.number = memberCount[teamId];
                SDL_snprintf(membersStr, sizeof(membersStr), "%s",
                             langGetTextFmt(STR_DLGLOBBY_TEAM_MEMBERS_N, &args));
            }
            const char *botsPart = "";
            if (botCount[teamId] == 1)      botsPart = langGetText(STR_DLGLOBBY_TEAM_1BOT);
            else if (botCount[teamId] > 1)  botsPart = langGetText(STR_DLGLOBBY_TEAM_NBOTS);
            const char *sep = botCount[teamId] > 0 ? " · " : "";
            ImGui::TextDisabled("%s%s%s", membersStr, sep, botsPart);
        }

        /* "Join Team" — moves the local player to this team. Available
         * to every client regardless of permissions (you can always
         * move yourself) and only rendered when you're not already on
         * this team. */
        if (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
            clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber != teamId) {
            ImGui::SameLine();
            char joinId[64];
            SDL_snprintf(joinId, sizeof(joinId), "%s##j%d", langGetText(STR_DLGLOBBY_JOIN_TEAM), teamId);
            if (ImGui::SmallButton(joinId)) {
                lobbySendTeamSet(cs,
                                 (uint8_t)myPlayerNum, (uint8_t)teamId);
            }
            if (ImGui::IsItemHovered()) {
                MessageArgs args = {};
                args.number = teamId;
                ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_JOIN, &args));
            }
        }

        /* Per-team "+ Bot" button (always visible to the host) plus an
         * optional "Bot Naming:" pool dropdown (only when the team has
         * at least one bot — picking a pool before any bot exists has
         * nothing to apply to) and an optional X (clear) button on
         * the right edge.
         *
         * The X is shown only for teams 3..15 (teams 1 and 2 are
         * persistent and never removable) and only when the team has
         * no humans on it — clearing a team with humans would orphan
         * them. We still reserve its width even when hidden so the
         * "+ Bot" button stays at the same X coordinate across teams
         * that do/don't render an X. */
        if (effectiveHost) {
            const float comboW   = 200.0f * s;
            /* Gap between the Bot Naming combo and the Add Bot button —
             * just a normal widget-pair spacing so the dropdown sits
             * directly next to Add Bot rather than being pushed off to
             * the middle of the row. */
            const float namingShift = 6.0f * s;
            const float botBtnW  = 95.0f * s;
            const float xBtnW    = 22.0f * s;
            const float labelW   = ImGui::CalcTextSize(langGetText(STR_DLGLOBBY_BOT_NAMING)).x;
            const float gap      = 6.0f * s;
            /* Bots are only addable when the server's AI policy allows
             * it (lobbyAiType != aiNone) AND the server has at least
             * one brain on disk to assign. Both fields are mirrored
             * from the server dynamically, so the button and the
             * Bot Naming controls disappear / reappear without a
             * reconnect when -ai policy or brains/ changes. */
            bool botsAllowed = (clientSimGetLobbyAiType(cs) != 0) &&
                               (clientSimGetLobbyBrainList(cs)->count > 0);
            bool showNaming = botsAllowed && botCount[teamId] > 0;
            int humanCount = memberCount[teamId] - botCount[teamId];
            bool showXBtn   = (teamId >= 3) && (humanCount == 0);
            /* Always reserve the X width so "+ Bot" sits at the same
             * X position across teams with/without an X. */
            /* When showing the Bot Naming controls, leave a wider
             * gap (namingShift) between the dropdown and the Add Bot
             * button so the label + dropdown sit further left and
             * the dropdown has room to be wider. */
            float effBotBtnW = botsAllowed ? botBtnW : 0.0f;
            float effBotGap  = botsAllowed ? gap    : 0.0f;
            float groupW = effBotBtnW + effBotGap + xBtnW
                         + (showNaming
                            ? labelW + gap + comboW + namingShift
                            : 0);
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - groupW);
            int curPool = clientSimGetLobbyTeamPool(cs, (BYTE)(teamId));
            if (curPool < 0 || curPool >= lobbyBotPoolCount()) curPool = 0;
            if (showNaming) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOT_NAMING));
                ImGui::SameLine(0.0f, gap);
                char poolId[16];
                SDL_snprintf(poolId, sizeof(poolId), "##pool%d", teamId);
                ImGui::SetNextItemWidth(comboW);
                if (ImGui::BeginCombo(poolId, lobbyBotPoolLabel(curPool))) {
                    for (int p = 0; p < lobbyBotPoolCount(); p++) {
                        bool sel = (p == curPool);
                        /* Disable pools already claimed by some other
                         * in_use team — server enforces uniqueness; the
                         * UI mirrors it so users see what's available. */
                        bool takenElsewhere = false;
                        for (int ot = 1; ot < MAX_TANKS; ot++) {
                            if (ot == teamId) continue;
                            if (clientSimGetLobbyTeamInUse(cs, (BYTE)ot) &&
                                clientSimGetLobbyTeamPool(cs, (BYTE)ot) == p) {
                                takenElsewhere = true;
                                break;
                            }
                        }
                        ImGuiSelectableFlags flags = takenElsewhere
                            ? ImGuiSelectableFlags_Disabled : 0;
                        if (ImGui::Selectable(lobbyBotPoolLabel(p), sel, flags)) {
                            const char *nameForMeta = clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))
                                ? clientSimGetLobbyTeamName(cs, (BYTE)(teamId)) : defaultName;
                            lobbySendTeamPool(cs, (uint8_t)teamId,
                                              (uint8_t)p, nameForMeta);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine(0.0f, namingShift);
            }
            /* Empty-label button used as a click target; the icon
             * and the "Add Bot" text are drawn ourselves on top via
             * WindowDrawList. This lets us shift the text right by
             * a precise pixel offset so the centered label clears
             * the overlaid bot-cpu glyph cleanly. */
            bool addBtnClicked = false;
            ImVec2 addBtnPos = ImGui::GetCursorScreenPos();
            bool addBtnPending = lobbyAddBotPending(cs);
            if (botsAllowed) {
                char addId[24];
                SDL_snprintf(addId, sizeof(addId), "##ab%d", teamId);
                if (addBtnPending) ImGui::BeginDisabled();
                addBtnClicked = ImGui::Button(addId, ImVec2(botBtnW, 0));
                if (addBtnPending) ImGui::EndDisabled();
            {
                float btnH = ImGui::GetFrameHeight();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                const char *addLbl = langGetText(STR_DLGLOBBY_ADDBOT_LBL);
                ImVec2 textSz = ImGui::CalcTextSize(addLbl);
                ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
                /* Centered horizontally, then shifted right by 10px
                 * so it never collides with the icon on the left. */
                ImVec2 textPos(addBtnPos.x + (botBtnW - textSz.x) * 0.5f
                                            + 10.0f * s,
                               addBtnPos.y + (btnH - textSz.y) * 0.5f);
                dl->AddText(textPos, textCol, addLbl);
            }
            /* Green for "your team" buttons, red for the others —
             * matches the bot-cpu glyph rendered per row. */
            uint8_t myTeamHdr = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS)
                                ? clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber : 0;
            SDL_Texture *addBtnIcon = (myTeamHdr != 0 && teamId == myTeamHdr)
                                      ? s_iconBotCpuGreen
                                      : s_iconBotCpuRed;
            if (addBtnIcon) {
                float btnH    = ImGui::GetFrameHeight();
                float imgSz   = ImGui::GetFontSize();
                /* Tucked a few pixels in from the left edge so the
                 * icon doesn't hug the button border. The button is
                 * wide enough that the centered "Add Bot" label
                 * clears the icon naturally. */
                ImVec2 iconPos(addBtnPos.x + 8.0f * s,
                               addBtnPos.y + (btnH - imgSz) * 0.5f);
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)addBtnIcon,
                    iconPos,
                    ImVec2(iconPos.x + imgSz, iconPos.y + imgSz));
            }
            }  /* end botsAllowed AddBot block */
            if (addBtnClicked) {
                /* When the first bot is added to a team, randomize
                 * the pool so the name comes from a varied source
                 * instead of always pool 0. Teams 1/2 are pre-marked
                 * in_use at server init, so gating on in_use never
                 * fired — gate on "no bots yet" instead. Subsequent
                 * bots in the same team reuse the chosen pool; the
                 * host can still override via the dropdown. */
                int effectivePool = curPool;
                if (botCount[teamId] == 0 && lobbyBotPoolCount() > 0) {
                    effectivePool = (int)bolo_rand_below((uint32_t)lobbyBotPoolCount());
                    const char *nameForMeta = clientSimGetLobbyTeamInUse(cs, (BYTE)(teamId))
                        ? clientSimGetLobbyTeamName(cs, (BYTE)(teamId)) : defaultName;
                    lobbySendTeamPool(cs, (uint8_t)teamId,
                                      (uint8_t)effectivePool, nameForMeta);
                }
                lobbySendAddBotDebounced(cs, effectivePool, (uint8_t)teamId);
            }
            if (ImGui::IsItemHovered()) {
                MessageArgs args = {};
                args.number = teamId;
                ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_ADDBOT, &args));
            }
            /* Pin the X to the right edge so it lines up across teams.
             * Use ImGui::CloseButton (imgui_internal.h) — the same
             * widget ImGui uses for window-close buttons. Renders a
             * proper crossed line "×" with hover + active styling,
             * sized to the current font. */
            if (botsAllowed) {
                ImGui::SameLine(0.0f, gap);
            }
            ImGui::AlignTextToFramePadding();
            if (showXBtn) {
                char rmStrId[24];
                SDL_snprintf(rmStrId, sizeof(rmStrId), "##rmt%d", teamId);
                ImGuiID rmId = ImGui::GetID(rmStrId);
                ImVec2 closePos = ImGui::GetCursorScreenPos();
                /* Vertically center the close button within the row
                 * height. CloseButton draws a FontSize × FontSize box;
                 * frame height is taller. */
                float frameH = ImGui::GetFrameHeight();
                float fs = ImGui::GetFontSize();
                closePos.y += (frameH - fs) * 0.5f;
                if (ImGui::CloseButton(rmId, closePos)) {
                    /* Clear the team's metadata and any bot members. */
                    for (int i = 0; i < MAX_TANKS; i++) {
                        if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected
                            && clientSimGetLobbySlot(cs, (BYTE)(i))->isBot
                            && clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber == teamId) {
                            lobbySendRemoveBot(cs, (uint8_t)i);
                        }
                    }
                    lobbySendTeamClear(cs, (uint8_t)teamId);
                }
                if (ImGui::IsItemHovered()) {
                    MessageArgs args = {};
                    args.number = teamId;
                    ImGui::SetTooltip("%s", langGetTextFmt(STR_DLGLOBBY_TOOLTIP_RMTEAM, &args));
                }
                /* CloseButton uses an explicit screen pos and only calls
                 * ItemAdd (no ItemSize), so the layout cursor is NOT
                 * advanced. Without this Dummy the following BeginTable
                 * starts at the cursor's pre-CloseButton X (right side
                 * of the header) and renders the entire bot table off
                 * to the right of the team panel with squished columns.
                 * The Dummy reserves the X-button's slot in the line so
                 * the next item line-wraps normally. */
                ImGui::Dummy(ImVec2(xBtnW, 0));
            } else {
                /* Hold the slot so + Bot's X position is stable. */
                ImGui::Dummy(ImVec2(xBtnW, 0));
            }
        }

        /* Member rows — proper ImGui Table.
         *
         * BordersInnerH gives us the inter-row dividers natively
         * (one between each row, none above the first / below the
         * last). RowBg + TableSetBgColor lets us paint the "you"
         * row a darker tint without manually computing rectangles.
         * SizingStretchProp makes the name column absorb leftover
         * space while the icon columns stay fixed.
         *
         * Column layout: [tank | name+icons | ping | ready | X]. */
        SDL_Renderer *r = sdl3DrawGetRenderer();
        uint8_t myTeam = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS)
                         ? clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->teamNumber : 0;
        char tableId[32];
        SDL_snprintf(tableId, sizeof(tableId), "##members%d", teamId);
        ImGuiTableFlags tableFlags = ImGuiTableFlags_BordersInnerH
                                   | ImGuiTableFlags_RowBg
                                   | ImGuiTableFlags_SizingStretchProp
                                   | ImGuiTableFlags_PadOuterX
                                   /* NoClip so the expanded bot's
                                    * AiConfig form (rendered in col 0)
                                    * is allowed to extend visually
                                    * across the other columns. */
                                   | ImGuiTableFlags_NoClip;
        if (ImGui::BeginTable(tableId, 7, tableFlags)) {
            /* Wide column 0 so the tank icon sits well inside the
             * team panel (not crammed against the left edge). The
             * icon is positioned with a leading SetCursorPosX nudge
             * inside the cell.
             *
             * Column 1 hosts the per-row identity icons (country
             * flag + platform/WBN/Steam badges for humans, the
             * bot-cpu glyph for bots) — extracted from the name
             * column so the name text always starts at a fixed X.
             *
             * Two stretch columns (name + spacer) with weights 3:1
             * park the ping/gear column at roughly 3/4 across the
             * row instead of flush against the ready/X cluster. */
            ImGui::TableSetupColumn("##tank",   ImGuiTableColumnFlags_WidthFixed, 60.0f * s);
            /* Wide enough for the worst case: country flag + platform +
             * WBN-verified shield + Steam badge (16 + 3×14 px plus
             * inter-icon spacing), so the verified badge can't spill into
             * the name column. */
            ImGui::TableSetupColumn("##icons",  ImGuiTableColumnFlags_WidthFixed, 96.0f * s);
            ImGui::TableSetupColumn("##name",   ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("##ping",   ImGuiTableColumnFlags_WidthFixed, 50.0f * s);
            ImGui::TableSetupColumn("##spacer", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##ready",  ImGuiTableColumnFlags_WidthFixed, 80.0f * s);
            ImGui::TableSetupColumn("##x",      ImGuiTableColumnFlags_WidthFixed, 44.0f * s);

            /* Drive striping ourselves (per-player, not per-table-row)
             * so the bot's expanded AiConfig sub-row inherits the same
             * background as its parent bot row — keeps the odd/even
             * stripe stable when the gear is toggled. */
            const ImU32 stripeA = ImGui::GetColorU32(ImGuiCol_TableRowBg);
            const ImU32 stripeB = ImGui::GetColorU32(ImGuiCol_TableRowBgAlt);
            int playerIdx = 0;

            for (int i = 0; i < MAX_TANKS; i++) {
                if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
                if (clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber != teamId) continue;

                bool isMe   = (i == myPlayerNum);
                bool isBot  = clientSimGetLobbySlot(cs, (BYTE)(i))->isBot;
                bool isSelf = isMe;
                bool isAlly = (myTeam != 0 && clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber == myTeam);

                /* Row height = the tallest single widget we render
                 * in the row plus a touch of breathing room. Widgets
                 * are positioned per-cell via absolute Y so each one
                 * is centered on the row's vertical midline regardless
                 * of its own height. */
                const float tankSz  = 18.0f * s;
                const float closeSz = ImGui::GetFontSize();
                const float rowH    = ImMax(ImGui::GetFrameHeight(),
                                            ImMax(tankSz, 22.0f * s));

                ImGui::TableNextRow(0, rowH);
                const ImU32 rowStripe = (playerIdx & 1) ? stripeB : stripeA;
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowStripe);
                if (isMe) {
                    /* Translucent dark tint that highlights the local
                     * player's row, painted on top of the stripe. */
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                           IM_COL32(0, 0, 0, 64));
                }
                playerIdx++;

                /* Per-cell vertical centering: capture the row's top
                 * Y on entering each cell, then set cursor.y to
                 * rowTopY + (rowH - widgetH)/2 immediately before
                 * every widget. SameLine() resets cursor.y to the
                 * line's min Y; we override it again per widget so
                 * widgets of different heights (flags 11px, icons
                 * 14px, text ~13px, tank 18px) each end up centered
                 * on the row midline. */
                float rowTopY;
                auto cyAbs = [&](float h) {
                    float y = rowTopY + (rowH - h) * 0.5f;
                    if (y < rowTopY) y = rowTopY;
                    ImGui::SetCursorPosY(y);
                };
                /* Text-specific centering: ImGui's text line box has a
                 * couple of pixels of "ascent whitespace" above the
                 * cap line and an almost-empty descender below, so a
                 * pure line-height-based centering makes the visible
                 * glyph mass sit too low (~8px top / ~5px bottom in a
                 * 22px row). Bias the cursor up by ~12% of font size
                 * (about 1.5–2 px) so the visible glyphs end up
                 * centered. */
                auto cyTextAbs = [&]() {
                    float h = ImGui::GetTextLineHeight();
                    float bias = ImGui::GetFontSize() * 0.12f;
                    float y = rowTopY + (rowH - h) * 0.5f - bias;
                    if (y < rowTopY) y = rowTopY;
                    ImGui::SetCursorPosY(y);
                };

                /* ── Column 0: drag handle + tank icon ─────────────── */
                ImGui::TableSetColumnIndex(0);
                rowTopY = ImGui::GetCursorPosY();
                /* Capture the row's screen-Y top while we're at it so
                 * later columns can do an absolute-rect hover test
                 * (gear visibility, etc.). */
                float rowTopScreenY = ImGui::GetCursorScreenPos().y;

                /* Drag handle — drives the "drag a player onto a team"
                 * flow. Authority: only effectiveHost (host / admin /
                 * openHost-empowered editor) can drag. Plain players
                 * change their own team via the per-team "Join" button
                 * to keep the row UI uncluttered.
                 * Server re-validates on PACKET_LOBBY_TEAM_SET.
                 * Rendered as a 4-arrow "move" cross centered in the
                 * row, before the tank icon. */
                bool canDragThis = effectiveHost;
                if (canDragThis) {
                    const float handleS = 14.0f * s;
                    cyAbs(handleS);
                    ImVec2 hPos = ImGui::GetCursorScreenPos();
                    char hId[24];
                    SDL_snprintf(hId, sizeof(hId), "##drag%d", i);
                    ImGui::InvisibleButton(hId, ImVec2(handleS, handleS));
                    ImU32 lineCol = ImGui::IsItemHovered()
                        ? IM_COL32(230, 230, 230, 230)
                        : IM_COL32(140, 140, 140, 200);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    float cx = hPos.x + handleS * 0.5f;
                    float cy = hPos.y + handleS * 0.5f;
                    float a  = handleS * 0.45f;
                    float h  = handleS * 0.20f;
                    float th = 1.2f;
                    /* Crossbars. */
                    dl->AddLine(ImVec2(cx - a, cy), ImVec2(cx + a, cy),
                                lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a), ImVec2(cx, cy + a),
                                lineCol, th);
                    /* Four arrowheads, one per crossbar tip. */
                    dl->AddLine(ImVec2(cx - a, cy),
                                ImVec2(cx - a + h, cy - h), lineCol, th);
                    dl->AddLine(ImVec2(cx - a, cy),
                                ImVec2(cx - a + h, cy + h), lineCol, th);
                    dl->AddLine(ImVec2(cx + a, cy),
                                ImVec2(cx + a - h, cy - h), lineCol, th);
                    dl->AddLine(ImVec2(cx + a, cy),
                                ImVec2(cx + a - h, cy + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a),
                                ImVec2(cx - h, cy - a + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy - a),
                                ImVec2(cx + h, cy - a + h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy + a),
                                ImVec2(cx - h, cy + a - h), lineCol, th);
                    dl->AddLine(ImVec2(cx, cy + a),
                                ImVec2(cx + h, cy + a - h), lineCol, th);
                    if (ImGui::BeginDragDropSource(
                            ImGuiDragDropFlags_SourceAllowNullID)) {
                        uint8_t slot = (uint8_t)i;
                        ImGui::SetDragDropPayload("WB_LOBBY_PLAYER",
                                                  &slot, sizeof(slot));
                        ImGui::TextUnformatted(
                            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]
                            ? clientSimGetLobbySlot(cs, (BYTE)(i))->playerName
                            : "(slot)");
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_DRAG));
                    }
                    ImGui::SameLine(0.0f, 4.0f * s);
                    /* Reset Y for the tank-icon block below. */
                    ImGui::SetCursorPosY(rowTopY);
                }
                {
                    SDL_Texture *tankTex = nullptr;
                    if (r) {
                        if (isSelf) {
                            tankTex = getTankSelf04Texture(r);  /* black self */
                        } else if (isAlly) {
                            tankTex = getTankGood04Texture(r);  /* green ally */
                            if (!tankTex) tankTex = getTankSelf04Texture(r);
                        } else {
                            tankTex = getTankEvil04Texture(r);  /* red enemy */
                        }
                    }
                    if (tankTex) {
                        /* Indent the tank inside its cell so it
                         * doesn't sit flush with the team panel's
                         * left edge. */
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 28.0f * s);
                        /* Apply the same optical bias used for text:
                         * the tank PNG has a sliver of empty pixels
                         * along its bottom (cannon points up), so a
                         * pure geometric center makes it sit visibly
                         * low in the row. Nudge up ~12% of font size
                         * to match the centered look of the labels. */
                        cyAbs(tankSz);
                        float tankBias = ImGui::GetFontSize() * 0.12f;
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - tankBias);
                        ImGui::Image((ImTextureID)tankTex, ImVec2(tankSz, tankSz));
                    }
                }

                /* ── Column 1: identity icons (flag/platform/bot) ── */
                ImGui::TableSetColumnIndex(1);
                rowTopY = ImGui::GetCursorPosY();
                if (isBot) {
                    /* Green for bots on the local player's team (incl.
                     * the local player's own bots), red for bots on
                     * any other team. Fall back to the neutral
                     * bot-cpu.svg if a tinted variant is missing.
                     * Drawn at the same size as the tank icon so the
                     * two badges line up visually across rows. */
                    SDL_Texture *botTex = isAlly ? s_iconBotCpuGreen
                                                 : s_iconBotCpuRed;
                    if (botTex) {
                        cyAbs(tankSz);
                        /* Visible pixel mass in the bot-cpu PNGs
                         * needs a small upward nudge to land
                         * vertically centered on the row. */
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
                        ImGui::Image((ImTextureID)botTex,
                                     ImVec2(tankSz, tankSz));
                    }
                } else {
                    /* Same 2px upward nudge applied to text / tank /
                     * chip / gear — keeps every glyph in the row
                     * landing on a consistent optical center. */
                    const float iconBiasY = 2.0f;
                    const char *cc = clientSimGetLobbySlot(cs, (BYTE)(i))->countryCode;
                    if (cc[0] != '\0' && !(cc[0] == 'X' && cc[1] == 'X') && flagsGetTexture(cc)) {
                        cyAbs((float)FLAG_HEIGHT);
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconBiasY);
                        drawCountryFlagWithTip(cc);
                        ImGui::SameLine();
                    }
                    /* All WBN/Steam/platform icons in renderPlayerName are
                     * WBN_ICON_SIZE tall — center them as one block. */
                    cyAbs((float)WBN_ICON_SIZE);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconBiasY);
                    {
                        /* Hide the WBN globe in local-only sessions —
                         * single-player has no WBN session and a LAN-only
                         * host would similarly skip WBN registration. Keep
                         * the Steam + platform badges since those signal
                         * what the player is running, not who they are on
                         * the leaderboard. */
                        uint8_t pflags = clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags;
                        if (clientSimIsSinglePlayer(cs) || clientSimIsLanOnly(cs)) {
                            pflags &= ~PLAYER_FLAG_WBN_VERIFIED;
                        }
                        renderPlayerName(NULL,
                                         pflags,
                                         clientSimGetLobbySlot(cs, (BYTE)(i))->clientType,
                                         "", false);
                    }
                }

                /* ── Column 2: name + inline tags ────────────────── */
                ImGui::TableSetColumnIndex(2);
                rowTopY = ImGui::GetCursorPosY();
                cyTextAbs();
                /* Truncate the name to its column width (less room reserved for
                 * a HOST/ADMIN/BOT tag) so it can't overflow and push the start
                 * dropdown into the Ready button on small windows. */
                {
                    bool rowHasTag = (i == clientSimGetLobbyHostSlot(cs)) || isBot ||
                        (!isBot && (clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags
                                    & PLAYER_FLAG_ADMIN));
                    float nameAvail  = ImGui::GetContentRegionAvail().x;
                    float tagReserve = rowHasTag ? 56.0f * s : 0.0f;
                    char nameBuf[64];
                    lobbyTruncateName(clientSimGetLobbySlot(cs, (BYTE)(i))->playerName,
                                      nameAvail - tagReserve, nameBuf, sizeof(nameBuf));
                    if (isBot) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              wbThemeColor(g_theme->botBadge));
                        ImGui::Text("%s", nameBuf);
                        ImGui::PopStyleColor();
                    } else if (isMe) {
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", nameBuf);
                    } else {
                        ImGui::Text("%s", nameBuf);
                    }
                }


                /* Inline tag pills after the name. Drawn via
                 * WindowDrawList so we can size them tightly and
                 * tint each one independently (HOST = yellow,
                 * BOT = muted blue-gray). Same pill recipe as the
                 * READY/NOT READY badge but at 70% font size. */
                auto drawNameTag = [&](const char *lbl, ImU32 bg, ImU32 fg,
                                       ImU32 border = 0) {
                    const float tagScale = 0.70f;
                    float tagFontSz = ImGui::GetFontSize() * tagScale;
                    ImVec2 baseSz = ImGui::CalcTextSize(lbl);
                    ImVec2 textSz(baseSz.x * tagScale, tagFontSz);
                    float padX = 6.0f * s;
                    float padY = 2.0f * s;
                    float pillW = textSz.x + padX * 2.0f;
                    float pillH = textSz.y + padY * 2.0f;
                    ImGui::SameLine(0.0f, 6.0f * s);
                    cyAbs(pillH);
                    /* Nudge HOST / BOT / ADMIN name-tags up 1px so
                     * they sit a touch above the row centerline,
                     * which lines them up better with the cap-height
                     * of the player name. */
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0f);
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    /* Square corners on the name tags so they read as
                     * "labels", not status pills like READY. */
                    dl->AddRectFilled(pos,
                                      ImVec2(pos.x + pillW, pos.y + pillH),
                                      bg, 0.0f);
                    if (border != 0) {
                        dl->AddRect(pos,
                                    ImVec2(pos.x + pillW, pos.y + pillH),
                                    border, 0.0f, 0, 1.0f);
                    }
                    dl->AddText(ImGui::GetFont(), tagFontSz,
                                ImVec2(pos.x + padX, pos.y + padY),
                                fg, lbl);
                    ImGui::Dummy(ImVec2(pillW, pillH));
                };
                if (i == clientSimGetLobbyHostSlot(cs)) {
                    /* Badge follows the current host slot. Themable bg /
                     * border / text triple lives in wb_theme.cpp. */
                    drawNameTag(langGetText(STR_DLGLOBBY_TAG_HOST),
                                g_theme->hostTagBg,
                                g_theme->hostTagText,
                                g_theme->hostTagBorder);
                }
                if (!isBot && i != clientSimGetLobbyHostSlot(cs) &&
                    (clientSimGetLobbySlot(cs, (BYTE)(i))->clientFlags & PLAYER_FLAG_ADMIN)) {
                    /* IP-matched admin (server -admins). Shown beside the
                     * name like HOST but in a distinct teal so it reads
                     * as a separate "host-level authority" badge. */
                    drawNameTag(langGetText(STR_DLGLOBBY_TAG_ADMIN),
                                IM_COL32(70, 160, 175, 255),
                                IM_COL32(10, 30, 35, 255));
                }
                if (isBot) {
                    drawNameTag(langGetText(STR_DLGLOBBY_TAG_BOT),
                                g_theme->botTagBg,
                                g_theme->botTagText,
                                g_theme->botTagBorder);
                }
                /* Track the rightmost name/tag edge across all rows so the
                 * start dropdowns can line up in a shared column. The last
                 * item drawn here is the name text or its trailing tag pill. */
                startColNameMaxRight = ImMax(startColNameMaxRight,
                                             ImGui::GetItemRectMax().x);

                /* ── Column 3: gear (bots) or ping (humans) ──────── */
                ImGui::TableSetColumnIndex(3);
                rowTopY = ImGui::GetCursorPosY();
                /* Gear visibility: always shown for bots when this
                 * client has lobby-edit authority (host / openHost /
                 * admin). Hidden entirely for non-permitted clients
                 * so they don't see a non-functional control. */
                if (isBot && effectiveHost) {
                    if (s_iconSettings) {
                        float iconSize = ImGui::GetFontSize();
                        cyAbs(iconSize);
                        /* settings.svg renders 5px above / 2px below
                         * with pure geometric centering — the gear
                         * sits slightly low in the row. Nudge up
                         * ~12% of font size (about 1.5px) so it
                         * matches the optical center used by the
                         * text and tank widgets. */
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY()
                                             - ImGui::GetFontSize() * 0.12f);
                        ImVec2 iconStart = ImGui::GetCursorScreenPos();
                        char btnId[24];
                        SDL_snprintf(btnId, sizeof(btnId), "##cfg%d", i);
                        bool clicked = ImGui::InvisibleButton(btnId,
                                                              ImVec2(iconSize, iconSize));
                        /* Tint the gear toward grey so it reads as a
                         * secondary action — the primary visual focus
                         * is the player name and status badges. Full
                         * white on hover so it lights up under the
                         * mouse. */
                        ImU32 gearTint = ImGui::IsItemHovered()
                            ? IM_COL32_WHITE
                            : IM_COL32(180, 180, 180, 200);
                        ImGui::GetWindowDrawList()->AddImage(
                            (ImTextureID)s_iconSettings,
                            iconStart,
                            ImVec2(iconStart.x + iconSize, iconStart.y + iconSize),
                            ImVec2(0, 0), ImVec2(1, 1), gearTint);
                        if (clicked) {
                            s_expandedBotSlot = (s_expandedBotSlot == i) ? -1 : i;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_CONFIG));
                        }
                    } else {
                        cyAbs(ImGui::GetFrameHeight());
                        char fallId[24];
                        SDL_snprintf(fallId, sizeof(fallId), "%s##cfg%d",
                                     s_expandedBotSlot == i ? "v" : ">", i);
                        if (ImGui::SmallButton(fallId)) {
                            s_expandedBotSlot = (s_expandedBotSlot == i) ? -1 : i;
                        }
                    }
                } else if (!isBot) {
                    if (clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs > 0) {
                        cyTextAbs();
                        ImVec4 pingColor;
                        if (clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs < 50)        pingColor = wbThemeColor(g_theme->statusOnline);
                        else if (clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs < 150)  pingColor = wbThemeColor(g_theme->statusHighPing);
                        else                                       pingColor = wbThemeColor(g_theme->statusDisconnected);
                        ImGui::TextColored(pingColor, "%dms", (int)clientSimGetLobbySlot(cs, (BYTE)(i))->pingMs);
                    }
                }

                /* ── Column 4: reserved map start, shown as the compass
                 * octant (N/NE/.../C) of where that start sits on the
                 * map, or — when the slot holds no reservation. Reuses
                 * the otherwise-empty spacer column so the label sits
                 * between the ping and ready cells without a table-wide
                 * column reshuffle. The octant comes from the cached
                 * s_startCompassId table (rebuilt on map change). */
                ImGui::TableSetColumnIndex(4);
                rowTopY = ImGui::GetCursorPosY();
                /* Spacer-column left edge == right edge of the ping/gear
                 * column. Used as a floor for the dropdown's left bound so
                 * it never overlaps the ping text / bot gear when names are
                 * short. Constant across rows; capture once. */
                if (startColPingRightX == 0.0f) {
                    startColPingRightX = ImGui::GetCursorScreenPos().x;
                }
                {
                    const ClientLobbySlot *cslot = clientSimGetLobbySlot(cs, (BYTE)(i));
                    uint8_t sIdx = cslot->startIdx;
                    /* A host edits any connected row; a non-host edits only
                     * its own. Everyone else sees the read-only compass.
                     * No optimistic apply — selecting just sends the
                     * command; the marker moves when CTRL_LOBBY_SLOT lands. */
                    bool canEditStart = cslot->connected && (effectiveHost || isMe);
                    if (!canEditStart) {
                        const char *startLbl = "—";
                        if (cslot->connected && sIdx != 0xFF &&
                            sIdx <= MAX_STARTS && s_startCompassId[sIdx] != 0) {
                            startLbl = langGetText(s_startCompassId[sIdx]);
                        }
                        cyTextAbs();
                        if (appliedStartCenterX > 0.0f) {
                            float tw = ImGui::CalcTextSize(startLbl).x;
                            ImVec2 sp = ImGui::GetCursorScreenPos();
                            sp.x = appliedStartCenterX - tw * 0.5f;
                            ImGui::SetCursorScreenPos(sp);
                        }
                        ImGui::TextDisabled("%s", startLbl);
                    } else {
                        char preview[64];
                        if (sIdx != 0xFF && sIdx <= MAX_STARTS &&
                            s_startCompassId[sIdx] != 0) {
                            SDL_snprintf(preview, sizeof(preview),
                                         "#%u \xC2\xB7 %s", (unsigned)sIdx,
                                         langGetText(s_startCompassId[sIdx]));
                        } else {
                            SDL_snprintf(preview, sizeof(preview), "%s",
                                         langGetText(STR_DLGLOBBY_START_UNASSIGNED));
                        }
                        const float comboW = 96.0f * s;
                        cyAbs(ImGui::GetFrameHeight());
                        if (appliedStartCenterX > 0.0f) {
                            ImVec2 sp = ImGui::GetCursorScreenPos();
                            sp.x = appliedStartCenterX - comboW * 0.5f;
                            /* Scootch left if the centered combo would overlap
                             * the Ready cell on the right. */
                            if (appliedReadyLeft > 0.0f &&
                                sp.x + comboW > appliedReadyLeft - 4.0f * s)
                                sp.x = appliedReadyLeft - 4.0f * s - comboW;
                            ImGui::SetCursorScreenPos(sp);
                        }
                        char comboId[24];
                        SDL_snprintf(comboId, sizeof(comboId), "##start%d", i);
                        ImGui::SetNextItemWidth(comboW);
                        if (ImGui::BeginCombo(comboId, preview)) {
                            for (int k = 1; k <= MAX_STARTS; k++) {
                                if (s_startCompassId[k] == 0) continue;
                                /* Connected holder of start k, if any. */
                                int holder = -1;
                                for (int h = 0; h < MAX_TANKS; h++) {
                                    const ClientLobbySlot *hs =
                                        clientSimGetLobbySlot(cs, (BYTE)h);
                                    if (hs->connected && hs->startIdx == k) {
                                        holder = h;
                                        break;
                                    }
                                }
                                bool occupiedByOther = (holder >= 0 && holder != i);
                                /* Non-host self-claim: only free starts + own. */
                                if (!effectiveHost && occupiedByOther) continue;
                                char entry[96];
                                if (occupiedByOther) {
                                    SDL_snprintf(entry, sizeof(entry),
                                                 "#%u \xC2\xB7 %s (%s)", (unsigned)k,
                                                 langGetText(s_startCompassId[k]),
                                                 clientSimGetLobbySlot(cs, (BYTE)holder)->playerName);
                                } else {
                                    SDL_snprintf(entry, sizeof(entry),
                                                 "#%u \xC2\xB7 %s", (unsigned)k,
                                                 langGetText(s_startCompassId[k]));
                                }
                                bool selected = (sIdx == (uint8_t)k);
                                if (ImGui::Selectable(entry, selected)) {
                                    clientSimNetSendLobbyClaimStart(cs, (BYTE)i, (BYTE)k);
                                }
                                /* Outline this start on the preview while its
                                 * dropdown entry is hovered. */
                                if (ImGui::IsItemHovered()) s_hoveredStartChoice = k;
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            bool relSel = (sIdx == 0xFF);
                            if (ImGui::Selectable(
                                    langGetText(STR_DLGLOBBY_START_UNASSIGNED), relSel)) {
                                clientSimNetSendLobbyClaimStart(cs, (BYTE)i, 0xFF);
                            }
                            if (relSel) ImGui::SetItemDefaultFocus();
                            ImGui::EndCombo();
                        }
                    }
                }

                /* ── Column 5: ready / not ready badge (humans only — bots
                 *   are always ready and don't need a pill) ─────────────── */
                ImGui::TableSetColumnIndex(5);
                rowTopY = ImGui::GetCursorPosY();
                /* Ready column geometry. The pill is right-aligned within
                 * this fixed-width column so it sits snug against the right
                 * of the row (just left of the kick-X), with extra breathing
                 * room after the start dropdown. matches TableSetupColumn. */
                const float readyColW   = 80.0f * s;
                const float readyInsetX = 4.0f * s;
                if (!isBot) {
                    bool isReady = clientSimGetLobbySlot(cs, (BYTE)(i))->ready;
                    const char *lbl = isReady ? langGetText(STR_DLGLOBBY_PILL_READY) : langGetText(STR_DLGLOBBY_PILL_NOTREADY);
                    /* Render the badge text at 80% of the row font
                     * size — a touch smaller than the player name
                     * so the pill reads as a status tag rather than
                     * a primary label. */
                    const float pillFontScale = 0.80f;
                    float pillFontSz = ImGui::GetFontSize() * pillFontScale;
                    ImVec2 baseSz = ImGui::CalcTextSize(lbl);
                    ImVec2 textSz(baseSz.x * pillFontScale, pillFontSz);
                    float padX  = 7.0f * s;
                    float padY  = 3.0f * s;
                    float pillW = textSz.x + padX * 2.0f;
                    float pillH = textSz.y + padY * 2.0f;
                    cyAbs(pillH);
                    /* Nudge the READY / NOT READY pill up 2px so it
                     * lines up with the cap-height of adjacent text
                     * rather than the row's geometric center. */
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
                    ImVec2 pillPos = ImGui::GetCursorScreenPos();
                    /* Right-align within the column; clamp so a very wide
                     * pill never spills out the left of the cell. */
                    float cellLeftX = pillPos.x;
                    pillPos.x = cellLeftX + readyColW - pillW - readyInsetX;
                    if (pillPos.x < cellLeftX) pillPos.x = cellLeftX;
                    /* The pill's left edge is the right bound of the gap the
                     * start dropdown centers in. Track the leftmost (widest
                     * pill) across rows so the dropdown clears every pill. */
                    if (startColReadyLeftX == 0.0f || pillPos.x < startColReadyLeftX)
                        startColReadyLeftX = pillPos.x;
                    ImU32 bgCol = isReady ? IM_COL32(42, 80, 44, 255)
                                          : IM_COL32(58, 58, 58, 255);
                    ImU32 fgCol = isReady ? IM_COL32(120, 210, 120, 255)
                                          : IM_COL32(180, 180, 180, 255);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(pillPos,
                                      ImVec2(pillPos.x + pillW,
                                             pillPos.y + pillH),
                                      bgCol, pillH * 0.5f);
                    dl->AddText(ImGui::GetFont(), pillFontSz,
                                ImVec2(pillPos.x + padX,
                                       pillPos.y + padY),
                                fgCol, lbl);
                    ImGui::Dummy(ImVec2(pillW, pillH));
                }

                /* ── Column 6: remove bot X / kick player X (CloseButton) ──
                 * Bots get an immediate remove; humans (non-self, non-host)
                 * stage a kick-confirm modal. Both visible only to host /
                 * admin / openHost — server re-validates either way. */
                ImGui::TableSetColumnIndex(6);
                rowTopY = ImGui::GetCursorPosY();
                if (isBot && effectiveHost) {
                    cyAbs(closeSz);
                    ImVec2 closePos = ImGui::GetCursorScreenPos();
                    char rbStr[24];
                    SDL_snprintf(rbStr, sizeof(rbStr), "##rb%d", i);
                    ImGuiID rbId = ImGui::GetID(rbStr);
                    if (ImGui::CloseButton(rbId, closePos)) {
                        lobbySendRemoveBot(cs, (uint8_t)i);
                        if (s_expandedBotSlot == i) s_expandedBotSlot = -1;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RMBOT));
                    }
                } else if (!isBot && !isMe && i != clientSimGetLobbyHostSlot(cs) && effectiveHost) {
                    cyAbs(closeSz);
                    ImVec2 basePos = ImGui::GetCursorScreenPos();
                    /* Host-only "Make host" promote button, drawn to the
                     * left of the kick X. openHost/admin (effectiveHost)
                     * can kick but must NOT transfer the host role, so
                     * this is gated on isLobbyHost, not effectiveHost. */
                    if (isLobbyHost(cs, myPlayerNum)) {
                        ImVec2 mhPos = basePos;
                        ImGui::SetCursorScreenPos(mhPos);
                        char mhStr[24];
                        SDL_snprintf(mhStr, sizeof(mhStr), "##mh%d", i);
                        bool mhClicked = ImGui::InvisibleButton(mhStr, ImVec2(closeSz, closeSz));
                        ImU32 mhTint = ImGui::IsItemHovered()
                            ? IM_COL32_WHITE : IM_COL32(180, 180, 180, 200);
                        ImDrawList *mhDl = ImGui::GetWindowDrawList();
                        /* Up-triangle "promote" glyph. Visual placeholder —
                         * the human may swap this for a crown later. */
                        ImVec2 apex(mhPos.x + closeSz * 0.50f, mhPos.y + closeSz * 0.20f);
                        ImVec2 bl  (mhPos.x + closeSz * 0.15f, mhPos.y + closeSz * 0.80f);
                        ImVec2 br  (mhPos.x + closeSz * 0.85f, mhPos.y + closeSz * 0.80f);
                        mhDl->AddTriangleFilled(apex, bl, br, mhTint);
                        if (mhClicked) {
                            s_makeHostPendingSlot = i;
                            SDL_strlcpy(s_makeHostPendingName,
                                        clientSimGetLobbySlot(cs, (BYTE)i)->playerName,
                                        sizeof(s_makeHostPendingName));
                            s_makeHostPendingOpen = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_MAKE_HOST));
                        }
                        /* Shift the kick X right so the two controls sit
                         * side by side in the column. */
                        basePos.x += closeSz + 4.0f * s;
                    }
                    /* CloseButton takes an explicit position and only calls
                     * ItemAdd (not ItemSize), so do NOT move the layout
                     * cursor here: a SetCursorScreenPos past the content max
                     * leaves ImGui's IsSetPos flag unvalidated and trips the
                     * "SetCursorPos to extend boundaries" assert at cell end.
                     * The InvisibleButton above already grew the cell. */
                    ImVec2 closePos = basePos;
                    char kbStr[24];
                    SDL_snprintf(kbStr, sizeof(kbStr), "##kb%d", i);
                    ImGuiID kbId = ImGui::GetID(kbStr);
                    if (ImGui::CloseButton(kbId, closePos)) {
                        s_kickPendingSlot = i;
                        SDL_strlcpy(s_kickPendingName,
                                    clientSimGetLobbySlot(cs, (BYTE)i)->playerName,
                                    sizeof(s_kickPendingName));
                        s_kickPendingOpen = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_KICK));
                    }
                }

                /* AiConfig sub-row when the wrench is expanded.
                 * Rendered as its own table row, but we only put
                 * content in column 0 — the form's widgets render
                 * past the column boundary and visually span the
                 * full team panel. The horizontal border below this
                 * row groups the form with the bot above it via the
                 * normal inter-row line. */
                if (isBot && s_expandedBotSlot == i && effectiveHost) {
                    ImGui::TableNextRow();
                    /* Inherit the parent bot row's stripe color so the
                     * sub-row reads as a continuation of that row and
                     * doesn't disturb the odd/even pattern. */
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowStripe);
                    ImGui::TableSetColumnIndex(0);
                    renderBotAiConfig(cs, i, teamId, s);
                }
            }
            ImGui::EndTable();
        }

        /* "+ Bot" button moved to the team header bar (right-aligned
         * next to the "Bot Naming:" pool dropdown) so all team-bot
         * controls live in one place. */

        ImGui::EndChild();
        /* Drop target: the team child window is the last item. Any
         * BeginDragDropSource elsewhere can drop a slot index here
         * to move that player onto this team. While a drag is over
         * the team, paint a translucent highlight along its border. */
        if (ImGui::BeginDragDropTarget()) {
            ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery;
            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
                "WB_LOBBY_PLAYER", flags);
            if (payload) {
                /* Paint a thicker, brighter border while the payload
                 * hovers the team child. */
                ImVec2 itemMin = ImGui::GetItemRectMin();
                ImVec2 itemMax = ImGui::GetItemRectMax();
                ImU32 hl = (tc & 0x00FFFFFF) | (0xC0 << 24);
                ImGui::GetForegroundDrawList()->AddRect(
                    itemMin, itemMax, hl, 4.0f, 0, 3.0f);
                if (payload->IsDelivery() &&
                    payload->DataSize == (int)sizeof(uint8_t)) {
                    uint8_t fromSlot = *(const uint8_t *)payload->Data;
                    if (fromSlot < MAX_TANKS) {
                        lobbySendTeamSet(cs,
                                         fromSlot, (uint8_t)teamId);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::Spacing();
    }

    /* Update the shared start-dropdown center from this frame's measured
     * bounds, for use next frame. The left bound is whichever sits further
     * right — the widest name(+tag) or the ping/gear column — plus a small
     * pad, so the dropdown is centered in the clear gap before Ready and
     * never collides with the ping text / bot gear. Guarded so a degenerate
     * frame (no rows, or no room) leaves the previous value untouched
     * rather than snapping the column. */
    if (startColReadyLeftX > 0.0f) {
        float startColLeftBound =
            ImMax(startColNameMaxRight, startColPingRightX) + 8.0f * s;
        if (startColReadyLeftX > startColLeftBound) {
            s_startComboCenterX = (startColLeftBound + startColReadyLeftX) * 0.5f;
        }
        s_startComboReadyLeft = startColReadyLeftX;  /* for next-frame clamp */
    }

    /* "Add Team" lives at the bottom of the team list so it reads as
     * "+ another team after these ones" rather than a header action.
     * Picks the lowest unused teamId and sends a default-name TEAM_META
     * (SP path writes the TeamMetadata directly since the wire packet
     * has no SP equivalent). */
    if (effectiveHost) {
        /* Full-width "Add Team" affordance, styled to read as a subtle
         * "+1 row" prompt rather than a primary action — translucent
         * background and dimmed text so it doesn't dominate the team
         * list it sits beneath. */
        ImVec4 baseBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 baseTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(baseTxt.x, baseTxt.y, baseTxt.z, baseTxt.w * 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f * s));
        bool addTeamClicked = ImGui::Button(langGetText(STR_DLGLOBBY_ADD_TEAM), ImVec2(-1, 0));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        if (addTeamClicked) {
            for (int t = 1; t < 16; t++) {
                if (memberCount[t] == 0 && !clientSimGetLobbyTeamInUse(cs, (BYTE)(t))) {
                    char defaultName[16];
                    {
                        MessageArgs args = {};
                        args.number = t;
                        SDL_snprintf(defaultName, sizeof(defaultName), "%s", langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args));
                    }
                    uint8_t color = (uint8_t)((t - 1) & 7);
                    clientSimNetSendLobbyTeamMeta(cs, (uint8_t)t,
                        color, 0 /*pool=classic*/, defaultName);
                    break;
                }
            }
        }
        ImGui::Spacing();
    }

    /* Unassigned tray. */
    if (unassignedCount > 0) {
        {
            MessageArgs args = {};
            args.number = unassignedCount;
            ImGui::TextDisabled("%s", langGetTextFmt(STR_DLGLOBBY_UNASSIGNED_FMT, &args));
        }
        for (int i = 0; i < MAX_TANKS; i++) {
            if (!clientSimGetLobbySlot(cs, (BYTE)(i))->connected) continue;
            if (clientSimGetLobbySlot(cs, (BYTE)(i))->teamNumber != 0) continue;
            ImGui::Bullet();
            if (i == myPlayerNum) {
                MessageArgs args = {};
                SDL_strlcpy(args.playerName,
                            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName,
                            sizeof(args.playerName));
                ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_YOU_FMT, &args));
            } else {
                ImGui::Text("%s", clientSimGetLobbySlot(cs, (BYTE)(i))->playerName);
            }
        }
    }

    /* No bottom team picker — Layout A relies on drag-to-assign + the
     * default-team logic on the server. Players who want to switch
     * teams can be dragged by the host (planned) or via context menu. */

    /* Deferred-open kick-confirm modal. OpenPopup must happen in the
     * same ID scope as BeginPopupModal, so we set a flag inside the
     * team child windows and pop it open here at the outer scope. */
    if (s_kickPendingOpen) {
        ImGui::OpenPopup("##kickConfirm");
        s_kickPendingOpen = false;
    }
    if (ImGui::BeginPopupModal("##kickConfirm", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            MessageArgs args = {};
            SDL_strlcpy(args.playerName, s_kickPendingName, sizeof(args.playerName));
            ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_KICK_FMT, &args));
        }
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_YES), ImVec2(80.0f * s, 0))) {
            if (s_kickPendingSlot > 0 && s_kickPendingSlot < MAX_TANKS) {
                clientSimNetSendLobbyKick(cs, (uint8_t)s_kickPendingSlot);
            }
            s_kickPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(80.0f * s, 0))) {
            s_kickPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s_makeHostPendingOpen) {
        ImGui::OpenPopup("##makeHostConfirm");
        s_makeHostPendingOpen = false;
    }
    if (ImGui::BeginPopupModal("##makeHostConfirm", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        {
            MessageArgs args = {};
            SDL_strlcpy(args.playerName, s_makeHostPendingName, sizeof(args.playerName));
            ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_MAKE_HOST_FMT, &args));
        }
        ImGui::Spacing();
        if (ImGui::Button(langGetText(STR_YES), ImVec2(80.0f * s, 0))) {
            if (s_makeHostPendingSlot >= 0 && s_makeHostPendingSlot < MAX_TANKS) {
                clientSimNetSendLobbyTransferHost(cs, (uint8_t)s_makeHostPendingSlot);
            }
            s_makeHostPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(langGetText(STR_CANCEL), ImVec2(80.0f * s, 0))) {
            s_makeHostPendingSlot = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* ── Layout A — bot AiConfig sub-row ──────────────────────────────
 * Inline panel under an expanded bot row showing the name override
 * with dice-reroll, difficulty dropdown, personality dropdown, and
 * close button. Edits dispatch as PACKET_LOBBY_BOT_CONFIG. */
static void renderBotAiConfig(ClientSim *cs,
                              int slot, int teamId, float s) {
    if (slot < 0 || slot >= MAX_TANKS) return;
    /* Render the whole AiConfig form at a smaller font so it reads
     * as a secondary control surface beneath the bot row. Saved +
     * restored so we don't bleed into the rest of the lobby. */
    float aicfgOldScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(aicfgOldScale * 0.85f);

    /* Build the "currently used names" array for the dice reroll —
     * collect every bot name in the lobby so we don't collide. */
    const char *usedNames[MAX_TANKS];
    int usedCount = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (clientSimGetLobbySlot(cs, (BYTE)(i))->connected && clientSimGetLobbySlot(cs, (BYTE)(i))->isBot &&
            clientSimGetLobbySlot(cs, (BYTE)(i))->playerName[0]) {
            usedNames[usedCount++] = clientSimGetLobbySlot(cs, (BYTE)(i))->playerName;
        }
    }

    /* Edit form sits flush with the left edge of the team panel —
     * no internal indent. (The caller already strips the surrounding
     * 50px row indent so the form spans the full team width.) */
    ImGui::PushID(slot);

    /* Two side-by-side groups: Name on the left, Bot Code on the right.
     * BeginGroup + SameLine works inside the outer table cell (which is
     * NoClip-enabled), unlike a nested BeginTable which gets clipped to
     * the parent's narrow column width and squashes the controls. */
    char nameBuf[32];
    strncpy(nameBuf, clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName, sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    bool nameChanged = false;
    bool diceClicked = false;
    const BrainList *bl = clientSimGetLobbyBrainList(cs);
    int pendingBrainPick = -1;

    /* Capture the form's top Y once and anchor every group there
     * explicitly via SetCursorScreenPos. Relying on SameLine alone
     * was producing a few-pixel drift (Name appearing higher than
     * Bot Code / Done) — probably because the Name group's
     * frame-bordered InputText shifts the group's "start Y" by
     * the FrameBorderSize when ItemAdd commits it. An explicit
     * anchor sidesteps that entirely. */
    ImVec2 formAnchor = ImGui::GetCursorScreenPos();
    /* Nudge all three sub-groups 2px down (purely cosmetic — the
     * AiConfig content felt visually crowded against the bot row
     * above). The end-of-function SetCursorScreenPos subtracts the
     * same nudge so the parent container's total height is
     * unchanged. */
    const float kFormNudgeY = 2.0f;
    formAnchor.y += kFormNudgeY;

    /* Name group. */
    ImGui::SetCursorScreenPos(formAnchor);
    ImGui::BeginGroup();
    ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_NAME));
    ImGui::SetNextItemWidth(180.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    nameChanged = ImGui::InputText("##botname", nameBuf, sizeof(nameBuf),
                                   ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar();
    ImGui::SameLine();
    diceClicked = ImGui::Button(langGetText(STR_DLGLOBBY_BOTCFG_REROLL));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_BOTCFG_REROLL_TIP));
    }
    ImGui::EndGroup();
    float nameGroupRightX = ImGui::GetItemRectMax().x;
    float nameGroupBottomY = ImGui::GetItemRectMax().y;

    /* Bot Code group, right of the Name group at the same anchor Y. */
    float botCodeStartX = 0.0f;
    float botCodeStartY = 0.0f;
    bool  botCodeShown  = (bl->count > 0);
    const float comboW  = 240.0f * s;
    if (botCodeShown) {
        float bcX = nameGroupRightX + 24.0f * s;
        ImGui::SetCursorScreenPos(ImVec2(bcX, formAnchor.y));
        botCodeStartX = bcX;
        botCodeStartY = formAnchor.y;
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_CODE));
        uint8_t curIdx = clientSimGetLobbyBotBrain(cs, (BYTE)(slot));
        const BrainListEntry *curEntry =
            (curIdx != 0xFF && curIdx < bl->count) ? &bl->entries[curIdx] : NULL;
        char preview[BRAIN_LIST_NAME_LEN + BRAIN_LIST_VER_LEN + 8];
        if (curEntry) {
            if (curEntry->version[0])
                SDL_snprintf(preview, sizeof(preview), "%s (%s)",
                             curEntry->name, curEntry->version);
            else
                SDL_snprintf(preview, sizeof(preview), "%s", curEntry->name);
        } else {
            SDL_snprintf(preview, sizeof(preview), "%s", langGetText(STR_DLGLOBBY_BOTCFG_NONE));
        }
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##botbrain", preview)) {
            for (int b = 0; b < bl->count; b++) {
                const BrainListEntry *e = &bl->entries[b];
                char label[BRAIN_LIST_NAME_LEN + BRAIN_LIST_VER_LEN + 8];
                if (e->version[0])
                    SDL_snprintf(label, sizeof(label), "%s (%s)",
                                 e->name, e->version);
                else
                    SDL_snprintf(label, sizeof(label), "%s", e->name);
                bool sel = (curEntry == e);
                if (ImGui::Selectable(label, sel)) {
                    pendingBrainPick = b;
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndGroup();
    }

    if (diceClicked) {
        /* Pick a name from this team's pool, excluding all currently-used
         * bot names (incl. this one — we want a NEW name, not the same).
         * Reroll = pool-driven, so clear the manual-override flag. */
        int pool = (teamId > 0 && teamId < 16) ? clientSimGetLobbyTeamPool(cs, (BYTE)(teamId)) : 0;
        char pickBuf[32];
        lobbyBotPoolPick(pool, usedNames, usedCount, pickBuf, sizeof(pickBuf));
        lobbySendBotConfig(cs, (uint8_t)slot,
            clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)), clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)), pickBuf);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = false;
    }
    if (nameChanged) {
        /* Manual edit — pin the name so a later pool change doesn't
         * overwrite it. */
        lobbySendBotConfig(cs, (uint8_t)slot,
            clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)), clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)), nameBuf);
        if (slot < MAX_TANKS) s_botNameOverridden[slot] = true;
    }
    if (pendingBrainPick >= 0 && pendingBrainPick < bl->count) {
        /* Stash as the sticky default so subsequent Add Bot clicks
         * inherit this choice instead of falling back to the server's
         * default brain. */
        s_lastChosenBrainIdx = (uint8_t)pendingBrainPick;
        lobbySendSetBotBrain(cs, (uint8_t)slot,
                             (uint8_t)pendingBrainPick);
    }

    /* Difficulty / Personality dropdowns are hidden for now — the
     * brain doesn't yet honor either field. Kept in the code (and
     * still wired through lobbySendBotConfig) so flipping this
     * flag to true is the only thing needed to re-enable the UI
     * once the brain consumes the values. */
    const bool kShowAiOptions = false;

    /* ── Difficulty dropdown ──────────────────────────────────── */
    if (kShowAiOptions) {
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_DIFFICULTY));
        ImGui::SameLine();
        const char *diffItems[] = { langGetText(STR_DLGLOBBY_BOTCFG_EASY),
                                    langGetText(STR_DLGLOBBY_BOTCFG_NORMAL),
                                    langGetText(STR_DLGLOBBY_BOTCFG_HARD) };
        int diff = clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot));
        if (diff < 0 || diff > 2) diff = 1;
        ImGui::SetNextItemWidth(90.0f * s);
        if (ImGui::Combo("##diff", &diff, diffItems, 3)) {
            lobbySendBotConfig(cs, (uint8_t)slot,
                (uint8_t)diff, clientSimGetLobbyBotPersonality(cs, (BYTE)(slot)),
                clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName);
        }
    }

    /* ── Personality dropdown ─────────────────────────────────── */
    if (kShowAiOptions) {
        ImGui::SameLine(0.0f, 16.0f * s);
        ImGui::TextDisabled("%s", langGetText(STR_DLGLOBBY_BOTCFG_PERSONALITY));
        ImGui::SameLine();
        const char *persItems[] = { langGetText(STR_DLGLOBBY_BOTCFG_NORMAL),
                                    langGetText(STR_DLGLOBBY_BOTCFG_AGGRESSIVE),
                                    langGetText(STR_DLGLOBBY_BOTCFG_DEFENSIVE),
                                    langGetText(STR_DLGLOBBY_BOTCFG_SNIPER) };
        int pers = clientSimGetLobbyBotPersonality(cs, (BYTE)(slot));
        if (pers < 0 || pers > 3) pers = 0;
        ImGui::SetNextItemWidth(110.0f * s);
        if (ImGui::Combo("##pers", &pers, persItems, 4)) {
            lobbySendBotConfig(cs, (uint8_t)slot,
                clientSimGetLobbyBotDifficulty(cs, (BYTE)(slot)), (uint8_t)pers,
                clientSimGetLobbySlot(cs, (BYTE)(slot))->playerName);
        }
    }

    /* ── Done group — anchored to formAnchor.y so all three sub-
     * groups (Name / Bot Code / Done) sit on the same top Y. The
     * "Hello" spacer above the button is a temporary debug label;
     * change back to a blank string once alignment is confirmed. */
    {
        const char *doneLbl = langGetText(STR_DLGLOBBY_BOTCFG_DONE);
        float doneW = ImGui::CalcTextSize(doneLbl).x
                    + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImVec2 winPos  = ImGui::GetWindowPos();
        float winRight = winPos.x + ImGui::GetWindowSize().x;
        float padR     = 20.0f * s;
        float targetScreenX = winRight - doneW - padR;
        ImGui::SetCursorScreenPos(ImVec2(targetScreenX, formAnchor.y));
        ImGui::BeginGroup();
        /* Invisible spacer that advances the cursor exactly the
         * same amount as the TextDisabled("Bot Code") above its
         * combo — using TextDisabled(" ") keeps the metrics
         * identical so the Done button below stays vertically
         * aligned with the combo. */
        ImGui::TextDisabled(" ");
        ImGui::SetCursorScreenPos(ImVec2(targetScreenX,
                                         ImGui::GetCursorScreenPos().y));
        if (ImGui::Button(doneLbl)) {
            s_expandedBotSlot = -1;
        }
        ImGui::EndGroup();
    }

    /* Restore the cursor below all three groups so any subsequent
     * widgets in the AiConfig sub-row land underneath. Subtract
     * kFormNudgeY from the final Y so the 2px we shifted the
     * contents down doesn't grow the parent container. */
    float bottomY = nameGroupBottomY;
    float curBotBottom = ImGui::GetItemRectMax().y;
    if (curBotBottom > bottomY) bottomY = curBotBottom;
    ImGui::SetCursorScreenPos(
        ImVec2(formAnchor.x,
               bottomY - kFormNudgeY + ImGui::GetStyle().ItemSpacing.y));

    ImGui::PopID();
    ImGui::Spacing();
    ImGui::SetWindowFontScale(aicfgOldScale);
}

/* ── Layout A — connectivity badge (icon + short text + Test btn) ─
 * Self-contained block extracted from the inline lobby chrome so it
 * can be rendered at top-right of the status bar (Layout A
 * placement) instead of below the settings line.
 *
 * Renders nothing when port-mapping status is DISABLED. Owns its own
 * popup modal for the detail view; clicking the icon/text opens it. */
static void renderConnectivityBadge(SDL_Renderer *renderer, float s) {
    ServerPortmapInfo pm;
    serverInstanceGetPortmapInfo(&pm);

    loadStatusIconsOnce(renderer, s);

    SDL_Texture *icon       = nullptr;
    const char  *shortText  = nullptr;
    ImVec4       color;
    const char  *detailFmt  = nullptr;

    switch (pm.status) {
        case SERVER_PORTMAP_DISABLED:
            return;  /* nothing to show */
        case SERVER_PORTMAP_PENDING:
            icon      = s_iconInfo;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_CHECKING);
            color     = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_PENDING);
            break;
        case SERVER_PORTMAP_SUCCEEDED:
            icon      = s_iconSuccess;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_ACCESSIBLE);
            color     = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_SUCCEEDED);
            break;
        case SERVER_PORTMAP_HOLE_PUNCH_OK:
            icon      = s_iconSuccess;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_ACCESSIBLE);
            color     = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_HOLE_PUNCH);
            break;
        case SERVER_PORTMAP_SYMMETRIC_NAT:
            icon      = s_iconError;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_UNREACHABLE);
            color     = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_SYMMETRIC);
            break;
        case SERVER_PORTMAP_FAILED:
            icon      = s_iconError;
            shortText = langGetText(STR_DLGLOBBY_PORTMAP_UNREACHABLE);
            color     = ImVec4(0.9f, 0.4f, 0.3f, 1.0f);
            detailFmt = langGetText(STR_DLGLOBBY_PORTMAP_DETAIL_FAILED);
            break;
    }

    if (!shortText) return;

    /* Right-align the badge to the available width so all states
     * ("Checking server reachability...", "Server accessible",
     * "Server unreachable") share the same right edge regardless of
     * text length. The caller still SameLine()s us onto the status
     * row; we just absorb the slack on our left. */
    float iconSize    = icon ? 18.0f * s : 0.0f;
    float spacingX    = icon ? ImGui::GetStyle().ItemSpacing.x : 0.0f;
    float textW       = ImGui::CalcTextSize(shortText).x;
    float badgeWidth  = iconSize + spacingX + textW;
    float availW      = ImGui::GetContentRegionAvail().x;
    if (availW > badgeWidth) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - badgeWidth));
    }

    ImGui::BeginGroup();
    if (icon) {
        ImGui::Image((ImTextureID)icon, ImVec2(iconSize, iconSize));
        ImGui::SameLine();
        float textOffset = (iconSize - ImGui::GetTextLineHeight()) * 0.5f;
        if (textOffset > 0.0f) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textOffset);
        }
    }
    ImGui::TextColored(color, "%s", shortText);
    ImGui::EndGroup();

    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    char popupTitle[128];
    SDL_snprintf(popupTitle, sizeof(popupTitle), "%s##portmap_details",
                 langGetText(STR_DLGLOBBY_PORTMAP_POPUP_TITLE));

    if (ImGui::IsItemClicked()) {
        ImGui::OpenPopup(popupTitle);
    }

    static bool s_pmOpen = true; s_pmOpen = true;
    if (ImGui::BeginPopupModal(popupTitle, &s_pmOpen,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ServerPortmapInfo pm2;
        serverInstanceGetPortmapInfo(&pm2);
        ImGui::PushTextWrapPos(420.0f * s);
        switch (pm2.status) {
            case SERVER_PORTMAP_SUCCEEDED: {
                MessageArgs args = {};
                SDL_strlcpy(args.string1, pm2.externalIp, sizeof(args.string1));
                args.number = (int)(unsigned)pm2.externalPort;
                ImGui::Text("%s",
                    langGetTextFmt(STR_DLGLOBBY_PORTMAP_DETAIL_SUCCEEDED, &args));
                break;
            }
            case SERVER_PORTMAP_SYMMETRIC_NAT:
            case SERVER_PORTMAP_FAILED: {
                MessageArgs args = {};
                args.number = (int)(unsigned)(pm2.internalPort != 0
                                              ? pm2.internalPort : 27500);
                ImGui::Text("%s",
                    langGetTextFmt(pm2.status == SERVER_PORTMAP_SYMMETRIC_NAT
                                       ? STR_DLGLOBBY_PORTMAP_DETAIL_SYMMETRIC
                                       : STR_DLGLOBBY_PORTMAP_DETAIL_FAILED,
                                   &args));
                break;
            }
            default:
                ImGui::TextUnformatted(detailFmt);
                break;
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        /* Live re-probe button + result lives inside the dialog now —
         * keeps the lobby header tidy and groups the test with the
         * status detail that describes what's being tested. */
        ManualProbeState mps = serverInstanceGetManualProbeState();
        /* Disable while the manual probe is in flight too — prevents the
         * user re-triggering a probe that hasn't reported back yet. */
        bool canTest = (pm2.status != SERVER_PORTMAP_DISABLED &&
                        pm2.status != SERVER_PORTMAP_PENDING &&
                        mps != MANUAL_PROBE_IN_PROGRESS);
        if (!canTest) ImGui::BeginDisabled();
        if (ImGui::Button(langGetText(STR_DLGLOBBY_TEST_CONNECTIVITY))) {
            serverInstanceTriggerManualProbe();
        }
        if (!canTest) ImGui::EndDisabled();
        if (!canTest &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_CONN_TEST_TIP));
        }
        if (mps != MANUAL_PROBE_IDLE) {
            ImGui::SameLine();
            switch (mps) {
                case MANUAL_PROBE_IN_PROGRESS:
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                       "%s", langGetText(STR_DLGLOBBY_TEST_TESTING));
                    break;
                case MANUAL_PROBE_SUCCESS:
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f),
                                       "%s", langGetText(STR_DLGLOBBY_TEST_REACHABLE));
                    break;
                case MANUAL_PROBE_TIMEOUT:
                    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.3f, 1.0f),
                                       "%s", langGetText(STR_DLGLOBBY_TEST_NO_REPLY));
                    break;
                case MANUAL_PROBE_IDLE:
                    break;
            }
        }
        /* [Close] only — affirmative "I'm done viewing", not cancel.
         * Use confirm slot so it gets default primary styling. */
        int closeFooter = WBUI::DialogFooter(/*cancelLabel*/ nullptr,
                                             /*confirmLabel*/ langGetText(STR_CLOSE));
        if (closeFooter != WBUI::FOOTER_NONE) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* ── Layout A — server reject toast ───────────────────────────────
 * Surfaces the last CTRL_COMMAND_REJECTED (delivered via
 * clientSimApplyControl) as a one-line orange status pill. Auto-clears
 * after the user dismisses it (clicks the X) so subsequent rejects
 * re-trigger naturally. */
static void renderLobbyRejectToast(ClientSim *cs, float s) {
    if (clientSimGetLobbyLastRejectPacket(cs) == 0) return;
    const char *reason = langGetText(STR_DLGLOBBY_REJECT_DEFAULT);
    switch (clientSimGetLobbyLastRejectReason(cs)) {
        case  1: reason = langGetText(STR_DLGLOBBY_REJECT_NOTHOST);        break;  /* LOBBY_REJECT_NOT_HOST */
        case  2: reason = langGetText(STR_DLGLOBBY_REJECT_LOCKED);         break;  /* LOBBY_REJECT_LOCKED */
        case  3: reason = langGetText(STR_DLGLOBBY_REJECT_INVALID);        break;  /* LOBBY_REJECT_INVALID */
        case  8: reason = langGetText(STR_DLGLOBBY_REJECT_BAD_STATE);      break;  /* CMD_REJECT_BAD_STATE */
        case  9: reason = langGetText(STR_NAME_INVALID_EMPTY);             break;  /* CMD_REJECT_NAME_EMPTY */
        case 10: reason = langGetText(STR_NAME_INVALID_RESERVED_PREFIX);   break;  /* CMD_REJECT_NAME_RESERVED_PREFIX */
        case 11: reason = langGetText(STR_NAME_INVALID_RESERVED_SUFFIX);   break;  /* CMD_REJECT_NAME_RESERVED_SUFFIX */
        case 12: reason = langGetText(STR_NAME_INVALID_MIXED_SCRIPTS);     break;  /* CMD_REJECT_NAME_MIXED_SCRIPTS */
        case 13: reason = langGetText(STR_NAME_INVALID_CHARS);             break;  /* CMD_REJECT_NAME_INVALID */
        case 14: reason = langGetText(STR_DLGSETNAME_INUSE_ERR);           break;  /* CMD_REJECT_NAME_TAKEN */
        case 15: reason = langGetText(STR_DLGLOBBY_REJECT_BOT_LIMIT);      break;  /* CMD_REJECT_BOT_LIMIT */
        default: break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, wbThemeColor(g_theme->lockBadge));
    {
        MessageArgs args = {};
        SDL_strlcpy(args.string1, reason, sizeof(args.string1));
        ImGui::Text("%s", langGetTextFmt(STR_DLGLOBBY_REJECT_FMT, &args));
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("X##rejdismiss")) {
        clientSimClearLobbyLastReject(cs);
    }
    (void)s;
}

/* ── Layout A — small inline lock badge ───────────────────────────
 * Renders an inline orange "[locked]" pill next to a setting name
 * when the server has flagged it in serverLocks. Cosmetic + tooltip. */
static void renderLockBadge(void) {
    ImGui::SameLine();
    if (s_iconLocked) {
        float sz = ImGui::GetTextLineHeight();
        ImGui::ImageWithBg((ImTextureID)s_iconLocked, ImVec2(sz, sz),
                          ImVec2(0, 0), ImVec2(1, 1),
                          ImVec4(0, 0, 0, 0),
                          wbThemeColor(g_theme->lockBadge));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, wbThemeColor(g_theme->lockBadge));
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_LOCK_BADGE));
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
    }
}

/* ── Layout A — editable game settings panel ──────────────────────
 * Renders the four mockup setting groups (Game Type / AI / Other /
 * Time Limit). Locked settings render disabled with a lock badge.
 * Edits dispatch as PACKET_LOBBY_SET_SETTING via the new wire
 * commands. Host-only or anyone if openHost. */
/* Forward decl — the form body is defined just after the panel, but the
 * panel (and the controller Settings tab) call it. */
static void renderGameSettingsBody(ClientSim *cs, int myPlayerNum, float s);

static void renderGameSettingsPanel(ClientSim *cs,
                                    int myPlayerNum, float s) {
    bool effectiveHost = isLobbyHost(cs, myPlayerNum) || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN));

    /* Non-privileged players get nothing — neither the form nor the
     * CollapsingHeader. The same map / game-type / pill-count /
     * AI-policy info is already on the lobby's top status bar, so
     * showing a disabled-out duplicate just clutters the view. */
    if (!effectiveHost) {
        return;
    }

    /* Drive the CollapsingHeader's open state explicitly so a "Hide
     * Settings" button at the bottom of the panel can fold it away
     * once the host is happy with the configuration.
     *
     * Default collapsed on the Steam Deck: its small screen needs the
     * vertical room for the player list and the Ready button, and the
     * map / game-type summary is already on the lobby's top status bar.
     * Desktop keeps it open. The chevron / "Hide Settings" button still
     * toggles it either way. */
    static bool s_settingsOpen     = true;
    static bool s_settingsOpenInit = false;
    if (!s_settingsOpenInit) {
        s_settingsOpen     = !uiModeIsSteamDeck();
        s_settingsOpenInit = true;
    }
    ImGui::SetNextItemOpen(s_settingsOpen, ImGuiCond_Always);
    /* Capture screen-Y of the header before drawing so the
     * right-aligned openHost control can be overlaid on the same
     * line via SetCursorScreenPos. */
    ImVec2 headerStart = ImGui::GetCursorScreenPos();
    bool headerOpen = ImGui::CollapsingHeader(langGetText(STR_DLGLOBBY_SETTINGS_HEADER));

    /* When the host has flipped on "Allow all players to change
     * settings", surface that on the header so non-host players
     * understand why the form is interactive for them. Right-anchored
     * within the header's row using the captured headerStart Y plus
     * the row's right edge minus the label width. */
    if (clientSimGetLobbyOpenHost(cs)) {
        const char *noteText = langGetText(STR_DLGLOBBY_OPENHOST_NOTE);
        ImVec2 textSize = ImGui::CalcTextSize(noteText);
        float headerH   = ImGui::GetFrameHeight();
        float availW    = ImGui::GetWindowContentRegionMax().x
                        - ImGui::GetWindowContentRegionMin().x;
        ImVec2 winPos   = ImGui::GetWindowPos();
        float pad       = ImGui::GetStyle().FramePadding.x;
        ImVec2 notePos(
            winPos.x + ImGui::GetWindowContentRegionMin().x + availW
                - textSize.x - pad,
            headerStart.y + (headerH - textSize.y) * 0.5f);
        ImGui::GetWindowDrawList()->AddText(
            notePos, IM_COL32(180, 180, 180, 220), noteText);
    }

    if (!headerOpen) {
        s_settingsOpen = false;
        return;
    }
    s_settingsOpen = true;

    /* The openHost ("Allow all players to change settings") state is
     * intentionally invisible to regular players — they shouldn't even
     * know they got their edit access via that particular toggle, only
     * that the settings happen to be interactable for them. The actual
     * editable checkbox is rendered for host/admin only in the "Other"
     * column below. */

    renderGameSettingsBody(cs, myPlayerNum, s);
}

/* The game-settings form proper (game type / AI policy / mines / time
 * limit / password). Split out of renderGameSettingsPanel so the
 * controller Settings tab can render it flat, without the desktop
 * collapsing-header chrome. Host-gated by every caller. */
static void renderGameSettingsBody(ClientSim *cs, int myPlayerNum, float s) {
    /* Same effective-host test the panel computes, recomputed here so the
     * per-control disabled state is identical whether the body renders in
     * the desktop collapsing header or the controller Settings tab. */
    bool effectiveHost = isLobbyHost(cs, myPlayerNum) || clientSimGetLobbyOpenHost(cs) ||
                         (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                          (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                           & PLAYER_FLAG_ADMIN));

    /* Settings body uses a smaller font than the rest of the lobby so
     * the 3-column form doesn't dominate the visual hierarchy. */
    float settingsOldScale = ImGui::GetCurrentWindow()->FontWindowScale;
    ImGui::SetWindowFontScale(settingsOldScale * 0.85f);

    ImGui::Spacing();
    ImGui::Columns(3, "##settingsCols", false);

    /* ── Game Type ──────────────────────────────────────────── */
    bool gtLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_GAME_TYPE) != 0;
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_GAMETYPE_LBL));
        if (gtLocked) renderLockBadge();
        bool disable = !effectiveHost || gtLocked;
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            langGetText(STR_DLGGAMESETUP_RADIO1),
            langGetText(STR_DLGGAMESETUP_RADIO2),
            langGetText(STR_DLGGAMESETUP_RADIO3),
        };
        bool rankedNow = clientSimGetLobbyRanked(cs);
        for (int i = 0; i < 3; i++) {
            /* gameType enum is 1-based (gameOpen=1, gameTournament=2,
             * gameStrictTournament=3), so the array index → enum
             * mapping is i+1. The previous (gameType)i comparison
             * read the wrong row as "checked" — Open showed as
             * Unknown, Tournament showed as Open, etc. */
            int enumVal = i + 1;
            /* Ranked games forbid the "Open" type — grey it out. */
            bool optDisabled = rankedNow && (gameType)enumVal == gameOpen;
            if (optDisabled) ImGui::BeginDisabled();
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##gt%d", items[i], i);
            bool checked = (clientSimGetLobbyGameType(cs) == (gameType)enumVal);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)enumVal;
                lobbySendSetting(cs, LST_GAME_TYPE, &v, 1);
            }
            if (optDisabled) ImGui::EndDisabled();
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Computer Players ────────────────────────────────────── */
    bool aiLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_AI_POLICY) != 0;
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_AI_SECTION_LBL));
        if (aiLocked) renderLockBadge();
        /* Ranked games force "No computer tanks" — disable the
         * whole AI block since none of the alternatives are valid. */
        bool disable = !effectiveHost || aiLocked
                        || clientSimGetLobbyRanked(cs);
        if (disable) ImGui::BeginDisabled();
        const char *items[] = {
            langGetText(STR_DLGLOBBY_AI_NONE),
            langGetText(STR_DLGLOBBY_AI_ALLOW),
            langGetText(STR_DLGLOBBY_AI_ADVANTAGE),
            langGetText(STR_DLGLOBBY_AI_FULLADV),
        };
        for (int i = 0; i < 4; i++) {
            /* Same trick as the Game Type radios — embed the label so
             * the whole row is clickable. */
            char rid[80];
            SDL_snprintf(rid, sizeof(rid), "%s##ai%d", items[i], i);
            bool checked = (clientSimGetLobbyAiType(cs) == (uint8_t)i);
            if (ImGui::RadioButton(rid, checked) && !checked) {
                uint8_t v = (uint8_t)i;
                lobbySendSetting(cs, LST_AI_POLICY, &v, 1);
            }
        }
        if (disable) ImGui::EndDisabled();
    }
    ImGui::NextColumn();

    /* ── Other (mines / time limit / autoLockOnGameStart) ────── */
    {
        ImGui::Text("%s", langGetText(STR_DLGLOBBY_OTHER_LBL));

        bool minesLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MINES) != 0;
        bool minesV = clientSimIsLobbyHiddenMines(cs);
        bool minesDisabled = !effectiveHost || minesLocked;
        if (minesDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_HIDDENMINES), &minesV)) {
            uint8_t v = minesV ? 1 : 0;
            lobbySendSetting(cs, LST_HIDDEN_MINES, &v, 1);
        }
        if (minesDisabled) ImGui::EndDisabled();
        if (minesLocked) renderLockBadge();

        /* "Allow all players to change settings" — toggles openHost
         * (the same flag that gates per-team manage-bots authority).
         * Visible to host / admin only. Regular players who got their
         * edit access via this very toggle don't see the control or
         * any read-only mirror of it — surfacing it would just
         * advertise the mechanism and tempt them to flip it off
         * (which they can't anyway, but the absence avoids the
         * noise). Sits next to "Allow Hidden Mines" so the related
         * authority-gating controls cluster together at the top of
         * the Other column. */
        if (!clientSimIsSinglePlayer(cs)) {
            bool isHostLocal = isLobbyHost(cs, myPlayerNum);
            bool isAdminLocal = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                                 (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                                  & PLAYER_FLAG_ADMIN));
            if (isHostLocal || isAdminLocal) {
                bool oh = clientSimGetLobbyOpenHost(cs);
                bool openHostLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_OPEN_HOST) != 0;
                if (openHostLocked) ImGui::BeginDisabled();
                if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_OPENHOST_CB),
                                    &oh)) {
                    clientSimNetSendLobbyOpenHost(cs, oh);
                }
                if (openHostLocked) ImGui::EndDisabled();
                if (openHostLocked) renderLockBadge();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (openHostLocked) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
                    } else {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_OPENHOST_TOOLTIP));
                    }
                }
            }
        }

        bool timeLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_TIME_LIMIT) != 0;
        bool timeV = clientSimGetLobbyTimeLimit(cs) > 0;
        bool timeDisabled = !effectiveHost || timeLocked;
        if (timeDisabled) ImGui::BeginDisabled();
        if (ImGui::Checkbox(langGetText(STR_DLGGAMESETUP_TIMELIMIT), &timeV)) {
            uint8_t v = timeV ? 1 : 0;
            lobbySendSetting(cs, LST_TIME_LIMIT, &v, 1);
        }
        if (timeV) {
            int mins = clientSimGetLobbyTimeLimit(cs) > 0
                ? (int)(clientSimGetLobbyTimeLimit(cs) / (50 * 60))
                : 30;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f * s);
            if (ImGui::InputInt("##tmin", &mins, 1, 5,
                                ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (mins < LOBBY_TIME_MINUTES_MIN) mins = LOBBY_TIME_MINUTES_MIN;
                if (mins > LOBBY_TIME_MINUTES_MAX) mins = LOBBY_TIME_MINUTES_MAX;
                uint8_t v[2] = { (uint8_t)((mins >> 8) & 0xFF),
                                 (uint8_t)(mins & 0xFF) };
                lobbySendSetting(cs, LST_TIME_MINUTES, v, 2);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_TIMELIMIT_MIN));
        }
        if (timeDisabled) ImGui::EndDisabled();
        if (timeLocked) renderLockBadge();

        /* Password protection — host or admin only (NOT openHost;
         * we don't want random connected players to be able to lock
         * the host out of their own server). MP only — SP has no
         * remote clients to keep out. */
        if (!clientSimIsSinglePlayer(cs)) {
            bool isHostLocal  = isLobbyHost(cs, myPlayerNum);
            bool isAdminLocal = (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                                 (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                                  & PLAYER_FLAG_ADMIN));
            if (isHostLocal || isAdminLocal) {
                /* Two pieces of UI state, both UI-layer-owned (the
                 * server never echoes the password value, so we
                 * can't reconstruct them from sim state):
                 *   s_pwOn   — the "Password" checkbox's intent.
                 *              Toggled directly by the click,
                 *              persists across frames.
                 *   s_pwBuf  — the typed password. When the user
                 *              unchecks we wipe it AND clear the
                 *              server-side value. */
                static char s_pwBuf[200] = {0};
                static bool s_pwOn       = false;

                bool pwLocked = (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_PASSWORD) != 0;
                if (pwLocked) ImGui::BeginDisabled();
                if (ImGui::Checkbox(langGetText(STR_DLGLOBBY_PASSWORD_CB), &s_pwOn)) {
                    if (!s_pwOn) {
                        s_pwBuf[0] = '\0';
                        clientSimNetSendLobbySetPassword(cs, "");
                    }
                    /* Checking with an empty buffer doesn't send
                     * anything yet — wait for the user to type. */
                }
                if (pwLocked) ImGui::EndDisabled();
                if (pwLocked) renderLockBadge();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (pwLocked) {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_TOOLTIP_RANKED_LOCKED));
                    } else {
                        ImGui::SetTooltip("%s", langGetText(STR_DLGLOBBY_PASSWORD_TOOLTIP));
                    }
                }
                if (s_pwOn && !pwLocked) {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(180.0f * s);
                    if (ImGui::InputText("##serverpw", s_pwBuf,
                                         sizeof(s_pwBuf),
                                         ImGuiInputTextFlags_Password |
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        clientSimNetSendLobbySetPassword(cs, s_pwBuf);
                    }
                    /* Send on blur too so the host doesn't have to
                     * remember to hit Enter. ImGui surfaces this via
                     * IsItemDeactivatedAfterEdit. */
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        clientSimNetSendLobbySetPassword(cs, s_pwBuf);
                    }
                }
            }
        }

        /* "Allow new players" / "Disallow new players once game has
         * started" moved to the team-list header (right of "+ Add Team")
         * so all join-related controls live in one row. See
         * renderTeamGroupedPlayers. */

    }

    ImGui::Columns(1);
    /* Restore the larger lobby-font scale before the Hide button so
     * its text isn't shrunk to the 0.85x the columns body uses. The
     * game-settings section's content auto-sizes to fit the columns
     * above — no explicit padding under it. */
    ImGui::SetWindowFontScale(settingsOldScale);

    /* "Hide Settings" — temporarily disabled. The form itself is
     * collapsible via its CollapsingHeader's chevron, so the
     * standalone button was redundant. Keeping the code in a
     * commented block so it's easy to restore if the section
     * tries to grow back. */
#if 0
    {
        const char *label = "Hide Settings";
        ImVec4 baseBtn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImVec4 baseTxt = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(baseBtn.x, baseBtn.y, baseBtn.z, baseBtn.w * 0.65f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(baseTxt.x, baseTxt.y, baseTxt.z, baseTxt.w * 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 2.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
        float btnW = ImGui::CalcTextSize(label).x + 16.0f * s;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btnW);
        if (ImGui::Button(label)) {
            s_settingsOpen = false;
        }
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);
    }
#endif
}

/* Render the lobby chat InputText + Send button pair, with the
 * 2-frame refocus-after-send and nav-highlight suppression logic
 * shared between the chat tab and the in-game lobby chat panel.
 * SetKeyboardFocusHere(0) is issued before the InputText (targeting
 * the next widget) rather than SetKeyboardFocusHere(-1) after the
 * Send button (which would target Send, not the input). The 2-frame
 * counter survives ImGui's internal InputText deactivation on Enter,
 * which stomps a single-frame focus request. */
static void lobbyRenderChatInputAndSend(ClientSim *cs, char *chatInput,
                                        BYTE myPlayerNum, bool hasTransport,
                                        float s, BYTE destPlayer) {
    float btnW = 60.0f * s;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btnW - 8.0f);
    if (s_chatRefocusFrames > 0) {
        ImGui::SetKeyboardFocusHere(0);
        s_chatRefocusFrames--;
    }
    if (s_chatHideNav)
        ImGui::GetCurrentWindow()->DC.NavHideHighlightOneFrame = true;
    bool enterPressed = ImGui::InputText("##ChatInput", chatInput, CHAT_INPUT_SIZE,
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    if (s_chatHideNav) {
        ImGui::GetCurrentContext()->NavCursorVisible = false;
        if (ImGui::IsItemActive()) s_chatHideNav = false;
    }
    ImGui::SameLine();
    bool chatEmpty = (chatInput[0] == '\0');
    if (chatEmpty) ImGui::BeginDisabled();
    bool sendClicked = ImGui::Button(langGetText(STR_DLGMSG_BUTTON), ImVec2(btnW, 0));
    if (chatEmpty) ImGui::EndDisabled();
    if ((sendClicked || enterPressed) && !chatEmpty && hasTransport) {
        clientSimNetSendChat(cs, destPlayer, chatInput);
        const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
        const char *myName = (mySlot && mySlot->connected)
            ? mySlot->playerName : langGetText(STR_DLGLOBBY_ME);
        if (CHAT_DEST_IS_TEAM(destPlayer))
            clientSimAppendLobbyTeamChat(cs, myName, chatInput);
        else
            clientSimAppendLobbyChat(cs, myName, chatInput);
        chatInput[0] = '\0';
        s_chatRefocusFrames = 2;
        s_chatHideNav = true;
    }
}

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
    dialogSetWindowSize(window, DIALOG_W, DIALOG_H);
    dialogSetWindowTitle(window, langGetText(STR_DLGLOBBY_WINTITLE));
    SDL_SetWindowResizable(window, true);
#endif
    dialogRestorePosition(window);
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

    /* Chat state */
    char chatInput[CHAT_INPUT_SIZE];
    chatInput[0] = '\0';

    /* Team combo items */
    const char *teamItems[] = {
        langGetText(STR_NONE), "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "10", "11", "12", "13", "14", "15", "16"
    };

    /* Map preview texture state */
    SDL_Texture *mapPreviewTex = NULL;
    bool mapPreviewBuilt = false;
    /* Signature of the start-ownership the current preview texture was built
     * with; drives in-place recolour when claims/teams change. The "seen"
     * pair debounces it so a burst of changes (e.g. bots auto-claiming starts
     * over several frames at SP startup) can't thrash the texture rebuild. */
    uint32_t mapPreviewOwnerSig = 0xFFFFFFFFu;
    uint32_t mapPreviewOwnerSeen = 0xFFFFFFFFu;
    int      mapPreviewOwnerStable = 0;
    bool prevMapDownloadComplete = clientSimIsMapDownloadComplete(cs);
    MapBounds mapBounds = {0, 0, MAP_PREVIEW_SIZE - 1, MAP_PREVIEW_SIZE - 1};
    /* Snapshot of the active map name; used to drop the stale
     * preview texture as soon as the map identity changes, even
     * before the UDP MAP_CHANGE re-download cycle arrives. */
    char prevMapName[128] = {0};
    {
        const char *curName = clientSimGetMapName(cs);
        if (curName) SDL_strlcpy(prevMapName, curName, sizeof(prevMapName));
    }
    /* Set when nameChanged fires (the in-process subscriber path,
     * instantly). Cleared when seqChanged fires (the UDP MAP_CHANGE
     * packet, a few frames later) — once we have the seq signal we
     * know the mapDownloadBuf is being reallocated, so the standard
     * !mapDownloadComplete "Downloading…" gate takes over. While
     * this is true the preview panel forces the "Downloading…"
     * placeholder so we don't briefly read "Map unavailable" in the
     * gap. SP-host never sets this (no transport → can't show a
     * preview anyway → don't get stuck on "Downloading…"). */
    bool awaitingMapChangePacket = false;
    /* Frame counter for the wait safety timeout. If the MAP_CHANGE
     * packet doesn't arrive within ~1 second of nameChanged firing
     * (e.g. server-side broadcast missed the host slot), we
     * force-clear the flag and rebuild from whatever bytes are
     * currently in the download buffer. */
    int awaitingFrames = 0;
    const int kAwaitingMaxFrames = 60;
    /* Last-seen MAP_CHANGE sequence number; bumps every time the
     * client receives PACKET_LOBBY_MAP_CHANGE. The "the mapDownload
     * is being replaced" edge — used together with the boolean
     * complete-transition edge to drive the texture rebuild. */
    uint32_t lastMapChangeSeq = clientSimGetLobbyMapChangeSeq(cs);

    /* Chat unread state — shared by the mobile tabbed layout and the
     * desktop chat tab bar so both flag a red tab when the chat tab you
     * are not viewing receives a new message. */
    bool chatUnread = false;
    int lastChatLen = 0;
    bool teamChatUnread = false;
    int lastTeamChatLen = 0;
    /* Active tab index for the tabbed layout (mobile, or desktop controller
     * mode). 0=Players, 1=Map, 2=Settings(host-only), 3=Chat(General),
     * 4=Team */
    int activeTab = 0;

    /* Query safe area insets for notch avoidance */
    DialogSafeInsets safeInsets = dialogGetSafeInsets(window);

    int result = 0;
    bool running = true;

    /* Controller mode: land the initial nav focus on the Ready button — the
     * action a pad user most wants on entry — instead of leaving focus
     * unset. Deferred until Ready is actually enabled (the map download may
     * still be in flight), and consumed once so the player can navigate away
     * freely afterwards. Armed only in controller mode; keyboard/mouse is
     * unaffected. */
#if !BOLO_MOBILE
    bool focusReadyPending = uiShouldUseControllerMode();
#endif

    /* Show the lobby in Steam immediately on entry; the throttled tick at
     * the top of the loop keeps the player count / connect address current
     * (e.g. once the host's external address resolves via the tracker). */
    gameFrontSetSteamPresenceLobby(cs);

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        gameFrontTickSteamPresenceLobby(cs);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
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

        /* Clear balance proposal when countdown starts */
        if (clientSimGetCountdownSeconds(cs) > 0 && clientSimIsBalanceProposalActive(cs)) {
            clientSimSetBalanceProposalActive(cs, false);
            clientSimClearBalanceProposal(cs);
        }

        /* Check for game start */
        if (clientSimGetNetStatus(cs) == netRunning) {
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
            if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; popupCompressedLen = 0; }
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
                int nOwn0 = lobbyComputeStartOwners(cs, (int)gameFrontGetPlayerNum(),
                                                    owners0, MAX_STARTS, &sig0);
                mapPreviewTex = buildMapPreview(renderer, mapData, mapLen, &mapBounds,
                                                nOwn0 ? owners0 : NULL, nOwn0);
                mapPreviewOwnerSig = sig0;
                if (prev) SDL_DestroyTexture(prev);
                WB_LOG_INFO(WB_LOG_CAT_GUI,
                    "[LOBBY/PREVIEW] rebuild done: tex=%p bounds=(%d..%d, %d..%d)",
                    (const void *)mapPreviewTex,
                    mapBounds.minX, mapBounds.maxX,
                    mapBounds.minY, mapBounds.maxY);
                /* Stash for popup decompression */
                if (popupCompressedData) { SDL_free(popupCompressedData); popupCompressedData = NULL; }
                popupCompressedData = (BYTE *)SDL_malloc(mapLen);
                if (popupCompressedData) {
                    SDL_memcpy(popupCompressedData, mapData, mapLen);
                    popupCompressedLen = mapLen;
                    /* Recompute the per-start compass cache from the new
                     * map bytes — only here, so the player-list column
                     * never decompresses the map per frame. */
                    rebuildStartCompassCache(popupCompressedData,
                                             popupCompressedLen);
                    /* If the user has the big map-preview popup open
                     * right now, refresh its underlying data in place
                     * so it seamlessly updates to the new map instead
                     * of closing on every server-side map change. */
                    if (mapPreviewPopupIsOpen()) {
                        mapPreviewPopupRefreshOpen(popupCompressedData,
                                                    popupCompressedLen);
                    }
                }
            }
            mapPreviewBuilt = true;
        }

        /* Recolour the preview in place when start ownership / teams change.
         * Claims don't trigger a map re-download, so the build-once path
         * above won't catch them. Cheap: rebuilds the 256² minimap only when
         * the ownership signature actually moves. */
        if (mapPreviewTex && popupCompressedData && popupCompressedLen > 0) {
            uint8_t owners[MAX_STARTS];
            uint32_t sig = 0;
            int nOwn = lobbyComputeStartOwners(cs, (int)gameFrontGetPlayerNum(),
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
                SDL_Texture *fresh = buildMapPreview(renderer, popupCompressedData,
                                                     popupCompressedLen, &mapBounds,
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

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();

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
                (myPlayerNum != 0) &&
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
            formatTimeLimit(clientSimGetLobbyTimeLimit(cs), timeStr, sizeof(timeStr));

#if BOLO_MOBILE
            /* Stack labels vertically on mobile so the line wraps cleanly. */
            ImGui::TextUnformatted(langGetText(STR_DLGNETINFO_SERVER));
            ImGui::SameLine();
            renderServerValue();
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), gameTypeStr(clientSimGetLobbyGameType(cs)));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        clientSimIsLobbyHiddenMines(cs) ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), aiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#else
            /* Leave button sits at the top-left, before the Server: line.
             * Escape key also opens the leave confirmation popup. Rendered as
             * a back arrow (left-pointing triangle) drawn into a normal-height
             * button so it matches Add Team / Ready visually without depending
             * on geometric-shape glyphs being present in the active font. */
            float leaveBtnH = ImGui::GetFrameHeight();
            float leaveBtnW = leaveBtnH * 1.4f;
            ImVec2 leaveBtnPos = ImGui::GetCursorScreenPos();
            bool leaveClicked = ImGui::Button("##leave", ImVec2(leaveBtnW, leaveBtnH));
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
            if (leaveClicked ||
                (((ImGui::IsKeyPressed(ImGuiKey_Escape) && !dialogNavWasInsideSubRegionAtFrameStart()) ||
                  (ImGui::IsKeyPressed(ImGuiKey_W) && IMGUI_PRIMARY_KEY_DOWN())
#ifdef __APPLE__
                  || (ImGui::IsKeyPressed(ImGuiKey_Period) && ImGui::GetIO().KeySuper)
#endif
                 ) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                char leavePopupId[64];
                SDL_snprintf(leavePopupId, sizeof(leavePopupId), "%s##lobby", langGetText(STR_DLGLOBBY_LEAVE_TITLE));
                ImGui::OpenPopup(leavePopupId);
            }
            ImGui::SameLine(0, 16);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(langGetText(STR_DLGNETINFO_SERVER));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            renderServerValue();
            ImGui::SameLine(0, 16);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_GAME_LBL), gameTypeStr(clientSimGetLobbyGameType(cs)));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_MINES_LBL),
                        clientSimIsLobbyHiddenMines(cs) ? langGetText(STR_DLGLOBBY_HIDDEN) : langGetText(STR_DLGLOBBY_VISIBLE));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), aiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
#endif

            /* Layout A — connectivity badge in the top-right corner.
             * Only meaningful when this client is also hosting AND
             * the host instance is actively NAT-punching (i.e. not
             * passed -no-natpunch / LAN-only). Skipping it covers SP,
             * LAN-only hosts, and non-hosting clients in one check. */
            if (serverInstanceIsNatPunchActive()) {
                /* renderConnectivityBadge right-aligns itself within the
                 * remaining horizontal space, so we just SameLine onto
                 * the status row and let it absorb the slack. */
                ImGui::SameLine();
                renderConnectivityBadge(renderer, s);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Layout A — surface the most recent server reject (locked
         * setting, non-host action, invalid request). Renders only
         * when clientSimGetLobbyLastRejectPacket(cs) != 0. */
        renderLobbyRejectToast(cs, s);

        /* Layout A — collapsible game settings panel (radios, checkboxes,
         * lock badges). Edits dispatch via PACKET_LOBBY_SET_SETTING.
         * Skipped entirely for non-privileged players — the same
         * info already lives in the top status bar, and the panel
         * is read-only anyway. */
        bool gsEffectiveHost = isLobbyHost(cs, myPlayerNum)
            || clientSimGetLobbyOpenHost(cs)
            || (myPlayerNum >= 0 && myPlayerNum < MAX_TANKS &&
                (clientSimGetLobbySlot(cs, (BYTE)(myPlayerNum))->clientFlags
                 & PLAYER_FLAG_ADMIN));
        /* Tabbed (controller) vs two-column (mouse) layout. Computed here so
         * the shared settings panel below renders only on the mouse path —
         * the tabbed layout shows settings in its own Settings tab instead. */
#if BOLO_MOBILE
        const bool useTabbedLobby = true;
#else
        const bool useTabbedLobby = uiShouldUseControllerMode();
#endif

        /* Settings above the layout is the two-column (mouse) path only; the
         * tabbed layout renders the same form in a dedicated tab, so skip it
         * here to avoid double-rendering it for the host. */
        if (gsEffectiveHost && !useTabbedLobby) {
            renderGameSettingsPanel(cs, myPlayerNum, s);
            ImGui::Separator();
            ImGui::Spacing();
        }

        /* Hosted-MP port-mapping status now renders at top-right via
         * renderConnectivityBadge — see the call site in the read-only
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
            if (!s_chooseMapOpen) {
                const ClientLobbySlot *myTabSlot =
                    clientSimGetLobbySlot(cs, myPlayerNum);
                bool onTeam = myTabSlot && myTabSlot->teamNumber != 0;
                int shift = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0)
                          - (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? 1 : 0);
                if (shift == 0)
                    shift = imguiSteamNavConsumeMenuTabShift();
                if (shift != 0) {
                    /* Visible tab indices in render order. The Settings
                     * predicate must match the Settings tab's BeginTabItem
                     * gate (gsEffectiveHost) exactly. */
                    int vis[5];
                    int nVis = 0;
                    vis[nVis++] = 0;                       /* Players */
                    vis[nVis++] = 1;                       /* Map */
                    if (gsEffectiveHost) vis[nVis++] = 2;  /* Settings */
                    vis[nVis++] = 3;                       /* Chat */
                    if (onTeam) vis[nVis++] = 4;           /* Team */
                    int cur = 0;
                    for (int i = 0; i < nVis; i++) {
                        if (vis[i] == activeTab) { cur = i; break; }
                    }
                    s_lobbyForceTab = vis[(cur + shift + nVis) % nVis];
                }
            }

            if (ImGui::BeginTabBar("##LobbyTabs")) {
                /* --- Players tab --- */
                if (ImGui::BeginTabItem(langGetText(STR_MENU_PLAYERS), nullptr,
                        s_lobbyForceTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    activeTab = 0;
                    /* Allow New Players row above the player list (mobile). */
                    renderAllowNewPlayersRow(cs, myPlayerNum, s);
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;
                    ImGui::BeginChild("##PlayerPanel", ImVec2(availW, tabH), ImGuiChildFlags_None);

                    /* Layout A: team-grouped player rendering. The
                     * legacy 5-column table below the #if 0 is left
                     * intact for reference; toggle the 0/1 to A/B
                     * compare during the in-progress UI rewrite. */
#if 1
                    bool isHostHere = isLobbyHost(cs, myPlayerNum);
                    renderTeamGroupedPlayers(cs, myPlayerNum, s, isHostHere);
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
                                    ImVec4 pingColor;
                                    if (slot->pingMs < 50)        pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                                    else if (slot->pingMs < 150)   pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                                    else                                        pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
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
                        s_lobbyForceTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    activeTab = 1;
                    float tabH = ImGui::GetContentRegionAvail().y - btnAreaH;

                    /* "Choose Map" — opens the separate chooser window.
                     * Host / admin / openHost-allowed only; non-privileged
                     * clients never see the button. Server enforces the
                     * same authority gate on PACKET_LOBBY_SET_MAP.
                     * effHostMap stays visible to the preview block below
                     * so privileged users can also click the preview to
                     * jump straight into the chooser. */
                    bool isHostLocal  = isLobbyHost(cs, myPlayerNum);
                    bool isAdminLocal = (myPlayerNum < MAX_TANKS &&
                        (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                         & PLAYER_FLAG_ADMIN));
                    bool effHostMap = isHostLocal || isAdminLocal ||
                                      clientSimGetLobbyOpenHost(cs);
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
                        ImGui::Image((ImTextureID)mapPreviewTex, ImVec2(innerSize, innerSize), uv0, uv1);
                        ImVec2 miniMin = ImGui::GetItemRectMin();
                        bool miniConsumed = lobbyPreviewInteract(cs, (int)myPlayerNum,
                                                effHostMap, miniMin, innerSize,
                                                bx0, by0, bx1, by1);
                        drawLobbyPreviewStartOverlay(cs, myPlayerNum, miniMin, innerSize,
                                                     bx0, by0, bx1, by1);
                        /* Reserve the full box so the gap also sits below. */
                        ImGui::SetCursorPosY(boxTopY + previewSize);
                        /* A click that didn't land on a start opens the zoomed
                         * popup (clicking a free start moves you there). */
                        if (popupCompressedData && !miniConsumed) {
                            mapPreviewPopupOnClick(popupCompressedData, popupCompressedLen,
                                                   mapBounds.minX, mapBounds.minY,
                                                   mapBounds.maxX, mapBounds.maxY);
                        }
                    } else if (!clientSimIsMapDownloadComplete(cs) || awaitingMapChangePacket) {
                        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_DOWNLOADING));
                        ImGui::Spacing();
                        float progress = (float)netGetDownloadPos() / 255.0f;
                        ImGui::ProgressBar(progress, ImVec2(-1, 20.0f * s));
                    } else {
                        ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MAP_UNAVAILABLE));
                    }
                    ImGui::Spacing();
                    ImGui::Text("%s - %dP %dB %dS", clientSimGetMapName(cs), clientSimGetLobbyPillCount(cs), clientSimGetLobbyBaseCount(cs), clientSimGetLobbyStartCount(cs));

                    /* Skip-map vote is gated by LOBBY_LOCK_MAP — locking
                     * the map blocks both manual change and skip-vote. */
                    if (clientSimIsMapSkipAvailable(cs) && clientSimIsInLobby(cs) &&
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
                        s_lobbyForceTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                    activeTab = 2;
                    ImGui::Spacing();
                    renderGameSettingsBody(cs, myPlayerNum, s);
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
                            s_lobbyForceTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
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

                        ImGui::BeginChild("##ChatHistory", ImVec2(0, chatHistH), ImGuiChildFlags_Borders);
                        /* Wrap long lines at the child's right edge so a
                         * full-length (128-char) message flows onto extra
                         * lines instead of running off the panel. */
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextUnformatted(clientSimGetLobbyChatHistory(cs));
                        ImGui::PopTextWrapPos();
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
                    BYTE myTeam = mySlot ? mySlot->teamNumber : 0;
                    if (myTeam != 0) {
                        bool teamTabColorPushed = false;
                        if (teamChatUnread) {
                            ImGui::PushStyleColor(ImGuiCol_Tab, ImVec4(0.5f, 0.0f, 0.0f, 1.0f));
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                            teamTabColorPushed = true;
                        }
                        if (ImGui::BeginTabItem(langGetText(STR_DLGLOBBY_CHAT_TEAM), nullptr,
                                s_lobbyForceTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
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

                            ImGui::BeginChild("##TeamChatHistory", ImVec2(0, chatHistH), ImGuiChildFlags_Borders);
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextUnformatted(clientSimGetLobbyTeamChatHistory(cs));
                            ImGui::PopTextWrapPos();
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

                ImGui::EndTabBar();
            }
            /* One-shot: the forced selection has been applied (or the bar
             * wasn't drawn this frame), so don't keep re-forcing it. */
            s_lobbyForceTab = -1;

            /* --- Bottom buttons (always visible) --- */
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPlayerNum);
                bool myReady = (mySlot && mySlot->connected) ? mySlot->ready : false;
                /* Block Ready when the lobby is flagged Ranked but the
                 * shape doesn't qualify. Ranked-ineligibility doesn't
                 * matter in SP / LAN where Ranked isn't shown at all. */
                bool rankedActive = !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs)
                                     && clientSimGetLobbyRanked(cs);
                RankedEligibility readyRe = rankedActive
                                              ? computeRankedEligibility(cs)
                                              : RankedEligibility{true, 0, 0, 0};
                bool rankedBlocksReady = rankedActive && !readyRe.sizesEligible;
                bool canReady = clientSimIsMapDownloadComplete(cs) && !rankedBlocksReady;

                if (!canReady) ImGui::BeginDisabled();
                const char *readyLabel = myReady ? langGetText(STR_DLGLOBBY_UNREADY) : langGetText(STR_DLGLOBBY_READY);
                if (myReady) {
                    ImGui::PushStyleColor(ImGuiCol_Button,         ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(0.10f, 0.45f, 0.10f, 1.0f));
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
                    rankedShapeTooltip(readyRe);
                }

                /* Legacy Balance Teams + Apply / Dismiss removed — the
                 * Balance-from-WBN affordance up top is the only entry
                 * point, and the server auto-applies WBN's split on
                 * response (no approval step). */

                ImGui::SameLine(0, 20);
                if (ImGui::Button(langGetText(STR_DLGLOBBY_LEAVE), ImVec2(100 * s, 0)) ||
                    (((ImGui::IsKeyPressed(ImGuiKey_Escape) && !dialogNavWasInsideSubRegionAtFrameStart()) ||
                      (ImGui::IsKeyPressed(ImGuiKey_W) && IMGUI_PRIMARY_KEY_DOWN())
#ifdef __APPLE__
                      || (ImGui::IsKeyPressed(ImGuiKey_Period) && ImGui::GetIO().KeySuper)
#endif
                     ) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup))) {
                    char leavePopupId[64];
                    SDL_snprintf(leavePopupId, sizeof(leavePopupId), "%s##lobby", langGetText(STR_DLGLOBBY_LEAVE_TITLE));
                    ImGui::OpenPopup(leavePopupId);
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
            float playerPanelW = availW - mapPanelW - 8.0f;

            /* "Allow New Players" row spans the full width above both
             * panels so PlayerPanel and MapPanel top edges align in Y. */
            float beforeAllowY = ImGui::GetCursorPosY();
            renderAllowNewPlayersRow(cs, myPlayerNum, s);
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
                bool isHostHere = isLobbyHost(cs, myPlayerNum);
                renderTeamGroupedPlayers(cs, myPlayerNum, s, isHostHere);
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
                            ImVec4 pingColor;
                            if (slot->pingMs < 50)        pingColor = ImVec4(0.0f, 0.9f, 0.0f, 1.0f);
                            else if (slot->pingMs < 150)   pingColor = ImVec4(0.9f, 0.9f, 0.0f, 1.0f);
                            else                                        pingColor = ImVec4(0.9f, 0.0f, 0.0f, 1.0f);
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
                BYTE myTeam = myChatSlot ? myChatSlot->teamNumber : 0;

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
                        /* Wrap long lines at the child's right edge (see the
                         * lobby Chat tab above). */
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextUnformatted(clientSimGetLobbyChatHistory(cs));
                        ImGui::PopTextWrapPos();
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
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextUnformatted(clientSimGetLobbyTeamChatHistory(cs));
                            ImGui::PopTextWrapPos();
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
            s_chatBlockMin = chatBlockCursor;
            s_chatBlockMax = ImVec2(chatBlockCursor.x + playerPanelW,
                                    chatBlockCursor.y + bottomH);

            ImGui::EndGroup(); /* /left column */

            ImGui::SameLine(0, 8.0f);

            /* Right column — MapPanel extends down to just above the
             * Ready/Balance footer, so the map preview's bottom border
             * sits on the same Y as the Ready button's top. */
            ImGui::BeginGroup();

            /* Right: Map preview + info */
            ImGui::BeginChild("##MapPanel", ImVec2(mapPanelW, mapH), ImGuiChildFlags_Borders);

            /* Prefer the existing texture even while we're waiting on
             * fresh bytes — the rebuild block above swaps it
             * atomically once the new map's chunks finish arriving,
             * so users keep seeing the previously-selected map until
             * the new one is ready to slot in. */
            if (mapPreviewTex) {
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
                bool isHostLocal  = isLobbyHost(cs, myPlayerNum);
                bool isAdminLocal = (myPlayerNum < MAX_TANKS &&
                    (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                     & PLAYER_FLAG_ADMIN));
                bool effHostMap = isHostLocal || isAdminLocal ||
                                  clientSimGetLobbyOpenHost(cs);
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
                ImGui::Image((ImTextureID)mapPreviewTex, ImVec2(innerSize, innerSize), uv0, uv1);
                ImVec2 miniMin = ImGui::GetItemRectMin();
                bool miniConsumed = lobbyPreviewInteract(cs, (int)myPlayerNum,
                                        effHostMap, miniMin, innerSize,
                                        bx0, by0, bx1, by1);
                drawLobbyPreviewStartOverlay(cs, myPlayerNum, miniMin, innerSize,
                                             bx0, by0, bx1, by1);
                /* Controller-reachable entry to the start picker: a focusable
                 * activation over the preview that opens the popup (which in
                 * controller mode shows the non-spatial start list). The pad
                 * has no click, so the mouse onClick path below can't reach
                 * it; Space/A on this item does. Mouse mode keeps the plain
                 * Image + click-to-open and skips this focusable overlap. */
                if (uiShouldUseControllerMode() && popupCompressedData) {
                    ImGui::SetCursorScreenPos(miniMin);
                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::InvisibleButton("##openStartPicker",
                                               ImVec2(innerSize, innerSize))) {
                        mapPreviewPopupOpenCompressed(popupCompressedData,
                                                      popupCompressedLen,
                                                      mapBounds.minX, mapBounds.minY,
                                                      mapBounds.maxX, mapBounds.maxY);
                    }
                }
                /* Reserve the full box so the gap also sits below the map. */
                ImGui::SetCursorPosY(boxTopY + previewSize);
                /* A click that didn't land on a start opens the zoomed popup
                 * (clicking a free start moves you there instead). Mouse only —
                 * controller opens it via the focusable overlap above. */
                if (popupCompressedData && !miniConsumed &&
                    !uiShouldUseControllerMode()) {
                    mapPreviewPopupOnClick(popupCompressedData, popupCompressedLen,
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
            } else if (!clientSimIsMapDownloadComplete(cs) || awaitingMapChangePacket) {
                /* No texture yet AND we're mid-download — show the
                 * progress bar so the user knows something's coming. */
                ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_DOWNLOADING));
                ImGui::Spacing();
                float progress = (float)netGetDownloadPos() / 255.0f;
                ImGui::ProgressBar(progress, ImVec2(-1, 20.0f * s));
                ImGui::Spacing();
            } else {
                ImGui::TextUnformatted(langGetText(STR_DLGLOBBY_MAP_UNAVAILABLE));
            }

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
                renderLockBadge();
            }
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_PILLBOXES), clientSimGetLobbyPillCount(cs));
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_BASES), clientSimGetLobbyBaseCount(cs));
            ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_STARTS), clientSimGetLobbyStartCount(cs));

            if (clientSimIsMapSkipAvailable(cs) && clientSimIsInLobby(cs) &&
                !(clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP)) {
                ImGui::Spacing();
                bool countdownActive = clientSimGetCountdownSeconds(cs) > 0;
                if (countdownActive) ImGui::BeginDisabled();
                bool voted = clientSimIsMapSkipMyVote(cs);
                const char *skipLabel = langGetText(
                    voted ? STR_DLGLOBBY_CANCELSKIP : STR_DLGLOBBY_SKIPMAP);
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
                    MessageArgs vargs;
                    memset(&vargs, 0, sizeof(vargs));
                    vargs.number  = skipCount;
                    vargs.number2 = humanCount;
                    ImGui::TextUnformatted(
                        langGetTextFmt(STR_DLGLOBBY_VOTES, &vargs));
                }
                if (countdownActive) ImGui::EndDisabled();
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
                bool myReady = (mySlot && mySlot->connected) ? mySlot->ready : false;
                /* Block Ready when the lobby is flagged Ranked but the
                 * shape doesn't qualify. Ranked-ineligibility doesn't
                 * matter in SP / LAN where Ranked isn't shown at all. */
                bool rankedActive = !clientSimIsSinglePlayer(cs) && !clientSimIsLanOnly(cs)
                                     && clientSimGetLobbyRanked(cs);
                RankedEligibility readyRe = rankedActive
                                              ? computeRankedEligibility(cs)
                                              : RankedEligibility{true, 0, 0, 0};
                bool rankedBlocksReady = rankedActive && !readyRe.sizesEligible;
                bool canReady = clientSimIsMapDownloadComplete(cs) && !rankedBlocksReady;

                /* No legacy Balance Teams button in the right column —
                 * the Balance-from-WBN affordance lives next to the
                 * Ranked checkbox up top, and the server auto-applies
                 * WBN's split immediately on response (no Apply /
                 * Dismiss approval step). */

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
                    rankedShapeTooltip(readyRe);
                }
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
            bool isHostLocal  = isLobbyHost(cs, myPlayerNum);
            bool isAdminLocal = (cs && myPlayerNum < MAX_TANKS &&
                (clientSimGetLobbySlot(cs, (BYTE)myPlayerNum)->clientFlags
                 & PLAYER_FLAG_ADMIN));
            bool effHostMap = isHostLocal || isAdminLocal ||
                              (cs && clientSimGetLobbyOpenHost(cs));
            /* Only offer "Choose map" in the popup when the user may
             * actually change it — same gate as the inline Choose Map
             * button (hidden when the server pins the map). */
            bool mapChangeAllowed = effHostMap &&
                !(cs && (clientSimGetLobbyServerLocks(cs) & LOBBY_LOCK_MAP));
            mapPreviewPopupSetShowChange(mapChangeAllowed);
            mapPreviewPopupSetStartPicker(cs, myPlayerNum, effHostMap);
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
                result = 0;
                running = false;
            } else if (f == WBUI::FOOTER_CANCEL) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

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
        if (s_chooseMapOpen &&
            s_chatBlockMax.x > s_chatBlockMin.x &&
            s_chatBlockMax.y > s_chatBlockMin.y) {
            /* No NoBringToFrontOnFocus / NoFocusOnAppearing here — the
             * scrim has to rise above the lobby's ##LobbyBg full-screen
             * window for its dark fill to actually show. The chooser
             * window is Begun right after this block and the user
             * interacts there, so ImGui's natural focus-follows-input
             * keeps the chooser on top of the scrim from frame two on. */
            const ImGuiWindowFlags scrimFlags =
                ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoNav |
                ImGuiWindowFlags_NoDocking;
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
                      (float)winW, s_chatBlockMin.y);
            /* Right strip — right of the chat, full remaining height. */
            drawScrim("##chooserScrimRight", s_chatBlockMax.x,
                      s_chatBlockMin.y,
                      (float)winW - s_chatBlockMax.x,
                      (float)winH - s_chatBlockMin.y);

            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            /* Focus the chooser on the frame it OPENS so it draws above
             * the scrim windows we just begun. Doing this every frame
             * snatches focus back from anything the user clicks into the
             * chat-hole (the chat InputText loses its cursor on the very
             * next frame). Once the chooser is on top, ImGui's natural
             * focus follows the click. */
            if (!s_chooseMapFocusedOnce) {
                ImGui::SetNextWindowFocus();
                s_chooseMapFocusedOnce = true;
            }
        } else {
            /* Reset the edge-trigger so re-opening focuses again. */
            s_chooseMapFocusedOnce = false;
        }

        /* Map chooser sub-window (Phase 1). Rendered after the main
         * lobby End() so it's a top-level ImGui window that the user
         * can drag around freely. Only renders when s_chooseMapOpen is
         * true, set by the "Choose Map" button on the Map tab.
         * Pass the *live* winW/winH (updated each frame from
         * SDL_GetWindowSize) — screenW/screenH is cached at lobby
         * entry and doesn't track OS-window resizes. */
        lobbyChooseMapRenderWindow(cs, renderer, s, winW, winH);

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

    /* Clean up map preview texture */
    if (mapPreviewTex) {
        SDL_DestroyTexture(mapPreviewTex);
    }
    if (popupCompressedData) {
        SDL_free(popupCompressedData);
        popupCompressedData = NULL;
        popupCompressedLen = 0;
    }
    mapPreviewPopupDestroy();

    /* Reset every file-scope flag that could render UI on the next
     * imguiLobbyShow if a disconnect (or any other exit) caught the
     * dialog mid-action. The MapChooserState instances themselves
     * stay populated as caches (next open re-uses the discovered
     * map list / preview view); the WBN HTTP caches stay too. Only
     * the visibility / focus / pending-action flags reset. The
     * cached ClientSim pointer also clears since the lobby that
     * captured it is being torn down. */
    s_chooseMapOpen             = false;
    s_chooseMapFocusedOnce      = false;
    s_chooseMapMaximized        = false;
    s_chooseMapWantCloseConfirm = false;
    s_chooseMapPreviewPending   = false;
    s_chooseMapCs               = NULL;

    /* Kick-confirmation modal — same bug class as the chooser
     * modal: a disconnect while the kick popup was up would leave
     * s_kickPendingOpen=true and the next lobby would render the
     * dialog over a fresh roster with a stale slot/name. */
    s_kickPendingOpen = false;
    s_kickPendingSlot = -1;
    s_kickPendingName[0] = '\0';
    s_makeHostPendingOpen = false;
    s_makeHostPendingSlot = -1;
    s_makeHostPendingName[0] = '\0';

    /* Add-bot debounce — the in-flight gate that disables the Add Bot
     * button until lobbyAddBotPending clears. If a click was in
     * flight at exit, next lobby would briefly disable Add Bot for
     * up to the debounce window. Plus the per-slot bot-name override
     * flags, which mark slots whose names were edited mid-session
     * and should not auto-rename when the team's naming pool
     * changes; carrying them into a fresh lobby would block legitimate
     * auto-renames on slots that the user never touched in this
     * session. */
    s_addBotSentMs        = 0;
    s_addBotExpectedConn  = 0;
    s_addBotFrame         = -1;
    memset(s_botNameOverridden, 0, sizeof(s_botNameOverridden));

    /* Cosmetic — whichever bot row had its AiConfig sub-row expanded
     * is meaningless once we're in a different lobby. Reset so the
     * next lobby starts with all rows collapsed. */
    s_expandedBotSlot = -1;

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
