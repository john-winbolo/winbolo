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

#include <cstring>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* ImGui::CloseButton — proper X widget with hit area */
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "imgui_nav_outline.h"
#include "imgui_controller_prompt.h"
#include "imgui_server_address.h"
#include "dialog_footer.h"
#include "../imgui_steam_nav.h"
#include "../glyphs.h"   /* glyphForActionAuto — controller footer legend */

#include "lobby/lobby_internal.h"
#include "../wbn_map_source.h"  /* wbnMapSourceResetDownload — drops a WinBolo.net map fetch left in flight */
extern "C" {
#include "../../../steam/steam_input_actions.h"  /* SI_ACTION_MENU_* names */
#include "../sdl3draw.h"
#include "../../gamefront.h"
#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "../../sound.h"
#include "server_sim.h"          /* serverSim* T1 wrappers for SP-host paths */
#include "../../../server/server_lifecycle.h"
#include "../sdl3imgui.h"
#include "../../ui_mode.h"
#include "../../../bolo/public/wire_limits.h"

#include "../map_preview_popup.h"
#include "../../lang.h"
#include "imgui_lobby.h"
#include "imgui_keyboard.h"
#include "imgui_keysetup.h"  /* the Key Setup popup, drawn and fed from this loop */
#include "imgui_messagebox.h"
#if defined(WINBOLO_VOICE)
#include "../../voice.h"
#include "../input.h"  /* inputPushToTalkPoll — the lobby reads the key itself */
#endif

}

/* From winbolo.c */
extern "C" {
  void windowFullScreenChoose(bool on);
}

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

    /* Seed the keyboard focus onto Ready / Start. Not controller-only any
     * more: with nothing seeded, ImGui's nav init picks the first item in the
     * window, and that is the little back arrow at the top left — so Enter in
     * the lobby asked to leave for the main menu rather than readying up.
     * Enter is the keyboard equivalent of the primary action, and here that is
     * Ready. Escape still leaves, which is the pair it belongs in.
     *
     * Applied once, on the first frame Ready is actually enabled — see the two
     * sites that consume this. SetKeyboardFocusHere carries
     * ImGuiNavMoveFlags_NoSetNavCursorVisible, so this seeds what Enter hits
     * without lighting a focus ring the player did not ask for. */
    s_lf.focusReadyPending = true;
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
/* Defined below, beside the poll that uses the same two records. */
static void lobbyBotAnnounceReset(void);

extern "C" void imguiLobbyFrameReset(void) {
    if (s_lf.mapPreviewTex) {
        SDL_DestroyTexture(s_lf.mapPreviewTex);
        s_lf.mapPreviewTex = NULL;
    }
    lobbyMapPreviewReset();
    mapPreviewPopupDestroy();

    lobbyChooserReset();
    lobbyScenarioChooserReset();

    lobbyChatReset();

    /* A lobby re-entered with a summary still stored should open on the
     * recap, not on whatever the last session was left looking at. */
    lobbyRecapReset();

#if !BOLO_MOBILE
    /* The reel holds the viewer's decoder singleton — never leave it running
     * past the lobby session. */
    lobbyReelEnd();
#endif
#if !BOLO_MOBILE && BOLO_RECAP_CLIP_GIF
    /* A clip export waiting on its save picker holds the whole encoded GIF,
     * and nothing polls it once the lobby is gone. Deliberately not folded
     * into lobbyReelEnd above: that also runs mid-session when the countdown
     * clears the summary, which must not throw away a clip the player is
     * still naming. */
    lobbyClipGifSaveAbandon();
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
    wbnMapSourceResetDownload();
#endif

    lobbyPlayersReset();

    lobbyCommandReset();

    /* A new lobby is a new audience: every bot's brain announces itself
       again. lobbyChatReset above already dropped the clickable blocks. */
    lobbyBotAnnounceReset();

    s_lf.active = false;
}

#if defined(WINBOLO_VOICE)
/* Voice starts off, so a player who has never turned it on has no reason to
 * know it is there. Said once, in the lobby chat, the first time somebody
 * else is heard, and it names the cog beside your own row as the way in.
 *
 * The talking set is read off the ClientSim, not the voice module: with voice
 * off this client is sent no voice frames at all, so the module's own map is
 * always empty. The server publishes this one whether or not this client is
 * listening, and it is the set each row's mic cell already reads.
 *
 * It is non-empty in the lobby and the countdown only — in a running game
 * voice follows the alliance and the server publishes an empty set on
 * purpose — so this cannot fire mid-game, which is the window the line wants.
 *
 * A file-static rather than a prefs key: a player who still has voice off on
 * the next run is worth telling once more. */
static bool s_voiceHintShown = false;

static void lobbyVoiceHintPoll(ClientSim *cs) {
    if (s_voiceHintShown || cs == NULL) return;
    if (voiceIsEnabled()) return;
    /* Nothing here to turn on where the server carries no voice. */
    if (voiceServerHasVoiceOff()) return;
    PlayerBitMap talking = clientSimGetVoiceTalkingMap(cs);
    /* With voice off you cannot be the one talking, but clear your own bit
       rather than leaving that to be worked out. A spectator holds no slot,
       so it has no bit of its own to clear. */
    BYTE me = clientSimGetMyPlayerNum(cs);
    if (me < MAX_TANKS) talking &= ~((PlayerBitMap)1u << me);
    if (talking == 0) return;
    clientSimAppendLobbyChat(cs, "***", langGetText(STR_DLGLOBBY_VOICE_HINT));
    s_voiceHintShown = true;
}
#endif

/* ── A bot's announce line in team chat ───────────────────────────────
 *
 * A brain may ship an announce.txt. When a bot running it is on YOUR team in
 * the lobby, that text goes into the TEAM chat as a line from the bot, and
 * the line opens the brain's commands.txt when clicked (lobby_chat.cpp).
 *
 * Said ONCE PER BRAIN, not once per bot: a four-bot team all on GoalHunter is
 * one message, not four. The per-slot record below is what makes that a
 * decision and not an accident — a slot is only looked at on the frame its
 * (bot, team, brain) shape changes, so the poll does no work at all on a
 * settled lobby, and a bot removed and re-added is looked at again.
 *
 * File-statics rather than prefs: a lobby re-entered is a fresh audience, and
 * lobbyChatDocsReset (through lobbyChatReset) clears the registry with them. */
static uint8_t s_announceSeen[MAX_TANKS];     /* brainIdx + 1, 0 = not seen */
static bool    s_announceBrain[BRAIN_LIST_MAX];

static void lobbyBotAnnounceReset(void) {
    memset(s_announceSeen, 0, sizeof(s_announceSeen));
    memset(s_announceBrain, 0, sizeof(s_announceBrain));
}

static void lobbyBotAnnouncePoll(ClientSim *cs) {
    if (cs == NULL || !clientSimIsInLobby(cs)) return;
    /* A spectator holds no slot, so it is on nobody's team and is told
     * nothing; team 0 is "unassigned" and is not a team either. */
    if (clientSimIsSpectator(cs)) return;

    const ClientLobbySlot *mine =
        clientSimGetLobbySlot(cs, clientSimGetMyPlayerNum(cs));
    if (mine == NULL || !mine->connected || mine->teamNumber == 0) return;

    const BrainList *bl = clientSimGetLobbyBrainList(cs);
    if (bl == NULL || bl->count <= 0) return;

    for (BYTE slot = 0; slot < MAX_TANKS; slot++) {
        const ClientLobbySlot *s = clientSimGetLobbySlot(cs, slot);
        uint8_t idx;
        uint8_t stamp;

        if (s == NULL || !s->connected || !s->isBot ||
            s->teamNumber != mine->teamNumber) {
            s_announceSeen[slot] = 0;      /* gone, or not ours any more */
            continue;
        }
        idx = clientSimGetLobbyBotBrain(cs, slot);
        if (idx == 0xFF || idx >= bl->count) idx = 0;   /* server default */
        stamp = (uint8_t)(idx + 1);
        if (s_announceSeen[slot] == stamp) continue;    /* already looked at */
        s_announceSeen[slot] = stamp;

        if (s_announceBrain[idx]) continue;             /* this brain spoke */
        {
            const char *announce = clientSimGetLobbyBrainAnnounce(cs, idx);
            char        base[BRAIN_LIST_NAME_LEN];
            char        line[LOBBY_CHAT_LINE_MAX];
            const char *history;
            bool        landed;

            if (announce == NULL || announce[0] == '\0') continue;

            /* The name on the line is the BOT's, so it reads like the bot
             * talking; the dialog is titled after the BRAIN, because the docs
             * belong to the brain and not to one bot. */
            brainListSplitVersion(bl->entries[idx].name, base, sizeof(base));
            if (clientSimFormatLobbyChatLine(line, sizeof(line),
                                             s->playerName, announce) < 0) {
                s_announceSeen[slot] = 0;               /* try again later */
                continue;
            }

            clientSimAppendLobbyTeamChat(cs, s->playerName, announce);

            /* Did it actually land? A chat buffer near full drops the append
             * without a word. The line is built by the same function the
             * append builds it with (client_sim.c), so the search cannot miss
             * for want of agreeing on the format. */
            history = clientSimGetLobbyTeamChatHistory(cs);
            landed  = (history != NULL && SDL_strstr(history, line) != NULL);

            if (!landed) {
                /* Nothing was said, so this brain has NOT spoken: leave the
                 * latch alone and un-stamp the slot, and the next frame says
                 * it again. Latching here was the bug — one dropped append
                 * and the brain's announce was gone for the whole lobby. */
                s_announceSeen[slot] = 0;
                continue;
            }
            s_announceBrain[idx] = true;

            /* Registering text that is not in the blob would simply never
             * match, so this waits on the same answer. The docs themselves
             * are not here yet: the server sends them when the line is
             * clicked, so the test is whether it has any to send. */
            if (clientSimLobbyBrainHasDocs(cs, idx)) {
                lobbyChatDocsRegister((int)idx, base, line);
            }
        }
    }
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

#if defined(WINBOLO_VOICE)
    /* Here rather than in either host's loop: both of them come through this
       function, so one call covers the blocking lobby and the in-game seam. */
    lobbyVoiceHintPoll(cs);
#endif

    /* Same place, same reason: a bot that has just joined your team says what
       its brain can do, once, in team chat. */
    lobbyBotAnnouncePoll(cs);

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
            BYTE spBuf[MAP_COMPRESSED_MAX_SIZE];
            if ((!mapData || mapLen <= 0) &&
                cs && !clientSimIsUdpTransport(cs)) {
                /* Non-UDP transport (SP host OR MP-host's own client)
                 * — no UDP buffer to pull from. Read the compressed
                 * map straight from the in-process ServerSim instead. */
                ServerSim *spSim = gameFrontGetSinglePlayerServerSim();
                if (spSim) {
                    mapLen  = serverSimGetCompressedMap(spSim, spBuf, (int)sizeof(spBuf));
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
            /* Own line like the labels around it — the smart-ping rule is
             * host-only in the settings panel, so this is where a joiner or
             * spectator reads it. */
            lobbyRenderSmartPingSummary(cs, s);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), lobbyAiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
            /* Own line again, and in the same place in the run as on the
             * desktop: whether the round runs mods is a host-only setting
             * everywhere else, so this is where a joiner or spectator is
             * told, and the hover is where the names are. */
            lobbyRenderModsSummary(cs, s);
            /* Own line, like the labels above it — the view policies are
             * the one part of the settings a joiner or spectator can see. */
            lobbyRenderVisibilitySummary(cs, s);
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
                (((ImGui::IsKeyPressed(ImGuiKey_Escape) && !mapPreviewPopupIsOpen() && !lobbyChooser()->open && !lobbyScenarioChooserIsOpen() && (uiShouldUseControllerMode() ? (!ImGui::GetIO().WantTextInput && !keyboardIsOpen()) : !dialogNavWasInsideSubRegionAtFrameStart())) ||
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
            /* Same reasoning as the view policies further down this line:
             * the settings panel that owns the checkbox is host-only, so
             * the header is where everyone else reads the rule. */
            ImGui::SameLine(0, 16);
            lobbyRenderSmartPingSummary(cs, s);
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_AI_LBL), lobbyAiTypeStr(clientSimGetLobbyAiType(cs)));
            ImGui::SameLine(0, 16);
            ImGui::Text("%s %s", langGetText(STR_DLGLOBBY_TIME_LBL), timeStr);
            /* Whether the round runs mods, on the same terms as the two
             * entries either side of it: the Mods Enabled checkbox and the
             * row that lists the names are both in the host-only settings
             * column, so everybody else reads the answer here and gets the
             * names off the hover. */
            ImGui::SameLine(0, 16);
            lobbyRenderModsSummary(cs, s);
            /* The view policies belong on this line because it is the one
             * place a joiner or spectator sees the host's settings — the
             * settings panel below is host-only. Before the connectivity
             * badge, which right-aligns into whatever space is left. */
            ImGui::SameLine(0, 16);
            lobbyRenderVisibilitySummary(cs, s);
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
            /* Also stood down while the scenario chooser is up, for the same
               reason as the Choose Map window above: a trigger press belongs
               to the dialog in front of the player, not to the tabs behind it.
               Losing the dialog is not the risk — it is drawn from the lobby's
               own frame rather than from any one tab's body, so it survives a
               tab change. The risk is the press going somewhere the player is
               not looking. */
            /* And while the details dialog is up. It is a modal, so it holds
               the pointer and the keyboard on its own, but a shoulder button
               is read here as a raw key and would cycle the tabs behind it. */
            if (!lobbyChooser()->open && !lobbyScenarioChooserIsOpen() &&
                !lobbyScenarioDetailsIsOpen()) {
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
                    /* Last frame's height of everything drawn under the
                     * preview, and where that block starts this frame
                     * (below 0 when no preview is drawn). */
                    static float tabMapBelowH = 0.0f;
                    float tabMapBelowTopY = -1.0f;

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

                        /* The height of what sits under the preview is
                         * measured on the previous frame rather than
                         * counted here: the scenario, mods and unsafe
                         * lines depend on what the round carries and wrap
                         * at the tab's width, and a count that falls
                         * short overflows the tab, whose scrollbar then
                         * narrows the width the preview is bound by and
                         * flips the layout every frame. The count is used
                         * only until there is a measurement. */
                        float infoH = tabMapBelowH > 0.0f
                                    ? tabMapBelowH
                                    : ImGui::GetTextLineHeightWithSpacing() * 2;
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
                        /* The two-team compass sits over the image, so it
                         * gets the mouse first; when it has it, the start
                         * claim/drag layer and the zoom popup stay out. */
                        bool compassHot = lobbyPreviewCompassHot(cs, effHostMap,
                                                miniMin, innerSize, gapPx, s);
                        bool miniConsumed = compassHot ||
                                            lobbyPreviewInteract(cs, (int)myPlayerNum,
                                                effHostMap, miniMin, innerSize,
                                                bx0, by0, bx1, by1);
                        lobbyDrawPreviewStartOverlay(cs, myPlayerNum, miniMin, innerSize,
                                                     bx0, by0, bx1, by1);
                        /* Painted last so the rose sits over any start label in its corner. */
                        lobbyDrawPreviewCompass(cs, effHostMap, miniMin, innerSize, gapPx, s);
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
                        tabMapBelowTopY = ImGui::GetCursorPosY();
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
                    ImGui::Text("%s - %dP %dB %dS", clientSimGetMapName(cs), lobbyLivePillCount(cs), lobbyLiveBaseCount(cs), lobbyLiveStartCount(cs));

                    /* What is playing, under what is loaded, for everyone —
                     * host included. These are the read-only lines and not
                     * the settings form's editable ones, which is what lets
                     * a host have both without reading the same thing twice:
                     * this panel says what the round is running, and the
                     * Server Settings column is where they change it. The
                     * Details button under them is the only way into the
                     * chooser a non-host has, so it comes with them here. */
                    lobbyRenderScenarioInfoLines(cs, s);

                    lobbyRenderMapSkipVote(cs, spectator, hasTransport, s, false);

                    if (tabMapBelowTopY >= 0.0f) {
                        tabMapBelowH = ImGui::GetCursorPosY() - tabMapBelowTopY;
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
                    /* Unless the player has already put the caret somewhere —
                     * the chat box, most likely, while a map was still coming
                     * down. Their choice wins, and the seed is dropped rather
                     * than held, so it cannot yank the caret out of a
                     * half-typed line the moment they pause. */
                    if (!ImGui::GetIO().WantTextInput) ImGui::SetKeyboardFocusHere();
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
                    (((ImGui::IsKeyPressed(ImGuiKey_Escape) && !mapPreviewPopupIsOpen() && !lobbyChooser()->open && !lobbyScenarioChooserIsOpen() && (uiShouldUseControllerMode() ? (!ImGui::GetIO().WantTextInput && !keyboardIsOpen()) : !dialogNavWasInsideSubRegionAtFrameStart())) ||
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

            /* The gutter has to clear the panels' touch padding on BOTH sides.
             *
             * Above 1.05x, dialogApplyScaling gives the lobby
             * TouchExtraPadding = 8 -- skipped on Deck, which keeps desktop
             * click feel. ImGui grows every child window's hit rect by that
             * padding when it picks the hovered window, so ##PlayerPanel
             * claimed this gap's left 8 pixels and ##MapPanel its right 8:
             * a flat 8 pixel gutter had none left for itself, and
             * FindHoveredWindowEx walks newest-first so the map panel won.
             * That is why the divider read as absent in full screen -- no
             * line, no resize cursor, no drag -- while windowed (a flat 1x, so
             * no touch padding) the same 8 pixels have always worked.
             *
             * The hovered window is decided in NewFrame from the style as it
             * stands then, so the padding cannot be pushed away around the two
             * BeginChild calls: width is the only lever. Carrying 2x the
             * padding leaves a reachable strip of exactly 8*s -- the windowed
             * target, scaled with everything around it -- and the line below
             * is drawn down the middle of that strip. Read from the style
             * rather than assumed, so Deck (padding 0, plain 8*s), desktop
             * full screen, tablet and mobile each come out right. */
            const float kTouchPad  = ImGui::GetStyle().TouchExtraPadding.x;
            const float kSplitterW = 8.0f * s + 2.0f * kTouchPad;

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
                float roomW  = availW - kSplitterW - kRecapLeftMinW * s;
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
                    ImMax(2.0f, 2.0f * s));
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
            /* Last frame's height of everything drawn under the preview,
             * and where that block starts this frame (below 0 when no
             * preview is drawn). */
            static float mapPanelBelowH = 0.0f;
            float mapPanelBelowTopY = -1.0f;
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
                 *   N = total vertical height of everything drawn under
                 *       the preview (Choose Map button, separator, the
                 *       map info rows, the scenario and mods lines, the
                 *       Skip Map vote row).
                 * N is last frame's measured height of that block, not a
                 * count: the scenario, mods and unsafe lines depend on
                 * what the round carries and wrap at the panel's width,
                 * and a count that falls short overflows the panel, whose
                 * scrollbar then narrows the width the preview is bound
                 * by and flips the layout every frame. The count below
                 * is used only until there is a measurement. */
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

                float N = mapPanelBelowH;
                if (N <= 0.0f) {
                    /* Choose Map button (right under the preview) */
                    if (effHostMap) N += frameH + spcH;
                    /* Spacing + Separator + Spacing */
                    N += spcH + 1.0f + spcH;
                    /* Map / pillboxes / bases / starts — 4 text rows */
                    N += 4.0f * lineH;
                    /* Skip Map button row (button + same-line votes text) */
                    if (skipAvail) N += spcH + frameH;
                }

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
                /* The two-team compass sits over the image, so it gets the
                 * mouse first; when it has it, the start claim/drag layer
                 * and the zoom popup stay out. */
                bool compassHot = lobbyPreviewCompassHot(cs, effHostMap,
                                        miniMin, innerSize, gapPx, s);
                bool miniConsumed = compassHot ||
                                    lobbyPreviewInteract(cs, (int)myPlayerNum,
                                        effHostMap, miniMin, innerSize,
                                        bx0, by0, bx1, by1);
                lobbyDrawPreviewStartOverlay(cs, myPlayerNum, miniMin, innerSize,
                                             bx0, by0, bx1, by1);
                /* Painted last so the rose sits over any start label in its corner. */
                lobbyDrawPreviewCompass(cs, effHostMap, miniMin, innerSize, gapPx, s);
                /* Reserve the full box so the gap also sits below the map. */
                ImGui::SetCursorPosY(boxTopY + previewSize);
                mapPanelBelowTopY = ImGui::GetCursorPosY();
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
                ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_PILLBOXES), lobbyLivePillCount(cs));
                ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_BASES), lobbyLiveBaseCount(cs));
                ImGui::Text("%s %d", langGetText(STR_DLGLOBBY_STARTS), lobbyLiveStartCount(cs));
                /* Same as the Map tab above, and directly under the start
                 * count for the same reason: the scenario and the mods are
                 * the last of what is loaded, and everyone reads them here
                 * whether or not they are the one who can change them, and
                 * everyone reaches the chooser from the Details button the
                 * lines end in. */
                lobbyRenderScenarioInfoLines(cs, s);

                lobbyRenderMapSkipVote(cs, spectator, hasTransport, s, false);

                if (mapPanelBelowTopY >= 0.0f) {
                    mapPanelBelowH = ImGui::GetCursorPosY() - mapPanelBelowTopY;
                }
            }

            /* Choose Map button moved up to sit directly under the
             * preview image (see the mapPreviewTex branch above). The
             * measured N height for the preview-sizing math includes
             * it. */

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
                    /* Unless the player has already put the caret somewhere —
                     * the chat box, most likely, while a map was still coming
                     * down. Their choice wins, and the seed is dropped rather
                     * than held, so it cannot yank the caret out of a
                     * half-typed line the moment they pause. */
                    if (!ImGui::GetIO().WantTextInput) ImGui::SetKeyboardFocusHere();
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

        /* --- A bot's brain docs, opened from its announce line in team
           chat. Here, at the lobby window's own id scope: the click that
           asks for it happens inside the chat child, and BeginPopupModal
           only finds a popup opened at its own scope. --- */
        lobbyChatDocsRenderModal(cs);

        /* --- What one scenario or mod is, opened from a script name in the
           Server Settings row or on the map panel, or from a row of the
           chooser. Here because no one scope sees all three: the row is drawn
           inside the settings form, the map panel's lines inside the map
           panel, and the chooser is a top-level window of its own drawn after
           this window has ended, so none of them can call OpenPopup where
           BeginPopupModal would find it. Each sets a flag and this is what
           turns it into a popup. --- */
        lobbyScenarioDetailsRenderModal(cs, s);

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
        if (lobbyChooser()->open &&
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
            if (!lobbyChooser()->focusedOnce) {
                ImGui::SetNextWindowFocus();
                lobbyChooser()->focusedOnce = true;
            }
        } else {
            /* Reset the edge-trigger so re-opening focuses again. */
            lobbyChooser()->focusedOnce = false;
        }

        /* Map chooser sub-window (Phase 1). Rendered after the main
         * lobby End() so it's a top-level ImGui window that the user
         * can drag around freely. Only renders when s_chooser.open is
         * true, set by the "Choose Map" button on the Map tab.
         * Pass the *live* winW/winH (updated each frame from
         * SDL_GetWindowSize) — screenW/screenH is cached at lobby
         * entry and doesn't track OS-window resizes. */
        lobbyChooseMapRenderWindow(cs, renderer, s, winW, winH);

        /* The scenario chooser, drawn here for the same reason and from the
         * same live winW/winH. Two buttons open it: the Details button on the
         * mods row inside the settings form's Server Settings column, which
         * only an effective host is shown, and the Details button under the
         * map panel's script lines, which everybody is shown — the dialog
         * draws itself read-only for a client that may not reorder the list.
         * Both of those sit inside something that can stop being drawn, a tab
         * on one layout and a collapsing header on the other, so drawing the
         * dialog from either would lose it the moment the player changed tab
         * or folded the settings header away. */
        lobbyScenarioChooserRenderWindow(cs, s, winW, winH);

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
 * mode leaves the host window alone, an active device preset forces its
 * own dimensions, and a fullscreen window is sized by the display —
 * saving any of them would overwrite the desktop size. */
static void lobbySaveWindowGeometry(SDL_Window *window) {
    if (!window) return;
#if !BOLO_MOBILE
    if (uiShouldUseControllerMode()) return;
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) return;
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

/* The UI scale for the surface the lobby window is on right now.
 * A windowed desktop lobby (and its nested map chooser / start picker) stays
 * at 1x -- scaling those fixed layouts is deferred to the lobby rework. A
 * fullscreen lobby keeps the height-derived scale so it isn't a 1x island on a
 * 1440p or 4K display. Read at entry and again whenever the window changes
 * surface under the lobby, so a full screen toggle mid-session lands on the
 * scale that surface would have opened at.
 * The WASM path has its own rule in lobbyFrameInitState: it never owns the
 * window, so it has no fullscreen state of its own to read. */
static float lobbyComputeUiScale(SDL_Window *window) {
    int screenW = 1024, screenH = 768;
    if (window) {
        SDL_GetWindowSize(window, &screenW, &screenH);
        if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    }
    float s = dialogComputeScale(screenW, screenH);
#if !BOLO_MOBILE
    if (!uiModeIsSteamDeck() &&
        !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
        s = 1.0f;
    }
#endif
    return s;
}

#if defined(WINBOLO_VOICE)
/*********************************************************
*NAME:          imguiLobbyPushToTalkPoll
*PURPOSE:
*  Reads the push-to-talk key for the lobby, once per turn
*  of whichever loop is running it.  Nothing else reads keys
*  here: the frontend tick step returns as soon as it sees
*  the lobby, so the in-game poll stops. Without this the
*  key never reaches the voice runtime in the lobby, and a
*  key still down as the game ended stays down for the whole
*  of it.
*
*  Reading input here means the window has keyboard focus
*  and nothing is taking typed characters. The lobby has a
*  chat box, and the letter push to talk sits on must not go
*  on the air while the player is typing it. WantTextInput
*  is the answer the last ImGui frame settled on — the
*  blocking loop polls this before its ImGui::NewFrame — and
*  a frame either way does not matter for a key held down.
*
*  A Key Setup row waiting for a key is the same case: the
*  keystroke is being aimed at a binding, not at the
*  microphone, and the key still bound to push to talk may
*  well be the one held down while it is rebound.
*
*  An unbound key is scancode 0, which inputPushToTalkPoll
*  never reports as held.
*********************************************************/
extern "C" void imguiLobbyPushToTalkPoll(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    keyItems lobbyKeys;
    windowGetKeys(&lobbyKeys);
    const bool reading =
        window != NULL &&
        (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0 &&
        !ImGui::GetIO().WantTextInput &&
        !imguiKeySetupIsCapturingInGameKey();
    inputPushToTalkPoll(&lobbyKeys, reading);
}
#endif

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

    /* Compute UI scale for the surface we are opening on */
    float s = lobbyComputeUiScale(window);

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
     * leaves the saved position untouched. Skipped entirely while the window
     * is fullscreen: its position and size are the display's, so clamping
     * from them would push the player's saved windowed geometry out of the
     * prefs file. */
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
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
#if !BOLO_MOBILE && !defined(__EMSCRIPTEN__)
            /* Alt+Enter, the same full screen key the game window takes. Ahead
               of the ImGui feed because Enter sends the chat line the player
               may be sitting in, and this keystroke is not for the field.
               The lobby holds a live chat buffer, a map preview and a recap
               view, so it cannot leave and come back the way the welcome
               screen does; instead it rebuilds the chrome that was read off
               the old surface -- the scale, the style metrics scaled with it,
               the font atlas both were rasterised for, and the safe insets. */
            if (dialogIsFullScreenToggleEvent(window, &ev)) {
                windowFullScreenChoose(
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
                s = lobbyComputeUiScale(window);
                /* Clear first: the font loaders only append. Reset the style
                   to a clean base too -- ScaleAllSizes compounds, so re-running
                   dialogApplyScaling over an already-scaled style would
                   double-count every metric. Same order as the entry setup, so
                   the UI font stays atlas font 0 and the countdown font 1. */
                io.Fonts->Clear();
                ImGui::GetStyle() = ImGuiStyle();
                ImGui::StyleColorsDark();
                imguiApplyBoloTheme();
                dialogApplyScaling(s);
                countdownFontSize = (s <= 1.05f) ? 54.0f : 60.0f * s;
                countdownFont     = imguiLoadBoloFontSized(countdownFontSize);
                s_lf.s                 = s;
                s_lf.countdownFont     = countdownFont;
                s_lf.countdownFontSize = countdownFontSize;
                s_lf.safeInsets        = dialogGetSafeInsets(window);
                continue;
            }
#endif
            /* Key Setup is open and a row is waiting for a key: the
               keystroke belongs to the binding, so it goes straight to the
               dialog and no further. Ahead of the ImGui feed because the
               lobby holds a chat input, and a key being bound must not be
               typed into it. */
            if (imguiKeySetupIsCapturingInGameKey() &&
                ev.type == SDL_EVENT_KEY_DOWN &&
                ev.key.windowID == SDL_GetWindowID(window)) {
                imguiKeySetupHandleInGameScancode((int)ev.key.scancode);
                continue;
            }
            /* Same for a controller row: a button or a trigger past half
               travel binds it, Escape drops the capture. Stick movement is
               ignored, so the row stays armed until one of those arrives. */
            if (imguiKeySetupIsCapturingInGamePad()) {
                if (ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    imguiKeySetupCancelInGamePad();
                    continue;
                }
                if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    imguiKeySetupHandleInGamePadButton((int)ev.gbutton.button);
                    continue;
                }
                if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                    (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                     ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
                    ev.gaxis.value > 16384 /* ~0.5 of 32767 */) {
                    imguiKeySetupHandleInGamePadTrigger((int)ev.gaxis.axis);
                    continue;
                }
            }
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
            if (dialogHandleQuitEvent(window, &ev)) {
                running = false;
            }
        }

        /* Tick transport to receive lobby packets */
        bool hasTransport = clientSimHasTransport(cs);
        if (hasTransport) {
            clientSimNetTick(cs);
        }
#if defined(WINBOLO_VOICE)
        /* Ahead of the tick below, so it sends what the key is doing this
         * turn of the loop rather than the last one's answer. */
        imguiLobbyPushToTalkPoll();
        /* This dialog owns the event loop while it is up, so the voice pump
         * the in-game loop runs each frame has to run here too - otherwise
         * lobby voice neither plays nor sends, and the mic-state command
         * does not reach the server until the game starts. */
        voiceTick(cs);
#endif

#if !BOLO_MOBILE && BOLO_RECAP_CLIP_GIF
        /* Write out a clip export whose save picker has been answered. Here
         * rather than in the recap that started it: the picker is asynchronous
         * so this loop keeps ticking the transport underneath it, and a player
         * who switches away from the recap while it is open must still get
         * their file. Outside the ImGui frame, because a failed write puts up
         * a message box that runs a loop of its own. */
        lobbyClipGifSavePoll();
#endif

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

        /* This dialog owns the frame while it is up, so the Key Setup popup
           the in-game frame draws has to be drawn here too — the same reason
           voiceTick runs from this loop. NULL rather than cs: the popup seeds
           its two checkboxes from a live tank when it is given one, and a
           lobby has no tank, only an idle slot. */
        imguiKeySetupRenderInGamePopup(NULL);

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

    /* The usual way out of here is the game starting, which breaks the loop
     * before another frame is drawn, and the running game's event pump reads
     * the same Key Setup capture state this loop was feeding. Drop it here, so
     * a dialog left open does not reappear over the game and an armed row does
     * not swallow the game's first keystroke. */
    imguiKeySetupCancelInGame();

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
