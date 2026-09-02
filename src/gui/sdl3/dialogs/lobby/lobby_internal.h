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
 * Name:          lobby_internal.h
 * Purpose:       The lobby's cross-translation-unit
 *                contract: feature macros for the parts of
 *                the recap a given build leaves out, the
 *                types and state accessors the lobby's
 *                sources share, declarations for every
 *                function called across a cluster boundary,
 *                and the hand-written declarations for the
 *                embedded log-viewer reel and the wasm
 *                round-log fetch.
 *********************************************************/

#ifndef LOBBY_INTERNAL_H
#define LOBBY_INTERNAL_H

/* Include this at file scope, never from inside an extern "C" block: the
 * header below pulls in imgui.h, whose templates do not compile with C
 * linkage. The declarations further down carry their own extern "C". */
#include "imgui_dialog_utils.h"

/* Parts of the recap that need something a given build does not have, and the
 * one that has a transport per platform. Named for what they need rather than
 * for the platform, so the reason each one is out reads at the site as well as
 * here.
 *
 * BOLO_REEL_WBN_FETCH      — the reel's WinBolo.net fallback source, used when
 *   a WBN-registered host uploads its round log instead of serving it over the
 *   game socket. On for every build: the retry ladder, the attempt cap, the
 *   size ceiling and the progress reporting are one copy, and only the
 *   transport underneath forks.
 * BOLO_REEL_WBN_FETCH_CURL — which transport that is. Desktop downloads on a
 *   std::thread through http.c. The browser has neither: http.c refuses to
 *   build under Emscripten because it embeds the WinBolo.net signing key, and
 *   the wasm client is single-threaded, so it drives the page's fetch()
 *   through src/wasm/round_log_fetch_wasm.c instead.
 * BOLO_RECAP_WBN_RATING    — the round's stars and comments, on the same
 *   http.c through wbn_comments; neither that nor the star widget is in the
 *   wasm target's sources.
 * BOLO_RECAP_CLIP_GIF      — the clip export. The GIF encoder's implementation
 *   TU (third_party/msf_gif/msf_gif_impl.c) is not in the wasm target's
 *   sources, and the save path spins its own SDL event loop waiting on a
 *   native file dialog, which a browser main loop cannot do. */
#define BOLO_REEL_WBN_FETCH 1
#ifdef __EMSCRIPTEN__
#define BOLO_REEL_WBN_FETCH_CURL 0
#define BOLO_RECAP_WBN_RATING    0
#define BOLO_RECAP_CLIP_GIF      0
#else
#define BOLO_REEL_WBN_FETCH_CURL 1
#define BOLO_RECAP_WBN_RATING    1
#define BOLO_RECAP_CLIP_GIF      1
#endif

/* The types below and the signatures further down name types that come from
 * elsewhere. They are pulled in here rather than left to each source to
 * include first, so this header parses on its own wherever it appears in an
 * include block. Each is wrapped in extern "C" because the two bolo headers
 * declare C functions without carrying their own linkage guards: a source
 * that includes this header before them would otherwise fix their
 * declarations at C++ linkage and fail to link.
 *
 * client_net.h        — CLIENT_ROUND_LOG_IDLE, the idle value LobbyReelState's
 *                       logState member is initialised to. Reaches
 *                       client_sim.h, and through it ClientSim (named by 40 of
 *                       the declarations below and held by LobbyChooserState),
 *                       BYTE (global.h), gameType (client_enums.h),
 *                       BRAIN_LIST_NAME_LEN / BRAIN_LIST_TAG_LEN /
 *                       BRAIN_LIST_DESC_LEN (brain_list.h, sizing
 *                       LobbyBrainMeta), and RoundStatsSummary +
 *                       HighlightWindow (round_stats.h).
 * types.h             — MAX_STARTS, sizing LobbyMapPreviewState's per-start
 *                       caches.
 * imgui_mapchooser.h  — MapPreviewPixels and MapChooserState, the two types
 *                       the map-chooser provider callbacks are declared
 *                       against. */
#ifdef __cplusplus
extern "C" {
#endif
#include "client_net.h"
#include "types.h"
#include "imgui_mapchooser.h"
#ifdef __cplusplus
}
#endif

/* Sizes shared between the cluster that owns the widget and the ones that
 * measure against it: the WinBolo.net row icon the player list draws, and the
 * chat input buffer the reel's timestamp helper appends into. */
#define LOBBY_WBN_ICON_SIZE 14
#define LOBBY_CHAT_INPUT_SIZE 129  /* 128 chars + null terminator */

/* ── Shared types ─────────────────────────────────────────────────
 * Types that appear in a signature or an accessor return below, so a caller
 * outside the cluster that owns them has to be able to name them. Everything
 * else the lobby defines stays private to the source that owns it.
 *
 * These are C++ types — default member initialisers, ImGui and SDL members —
 * so they sit outside the extern "C" block at the end of this header. */

/* Client-side cache of per-brain about.txt metadata, keyed by catalogue name.
 * brainListLoadMeta hits disk, so the combo would otherwise re-read every frame
 * while open. Entries are loaded lazily and never invalidated (the brains/ tree
 * doesn't change at runtime). */
struct LobbyBrainMeta {
    char name[BRAIN_LIST_NAME_LEN];
    char tagline[BRAIN_LIST_TAG_LEN];
    char desc[BRAIN_LIST_DESC_LEN];
};

/* Bounding box of interesting (non-sea) terrain in the map preview */
struct LobbyMapBounds {
    int minX, minY, maxX, maxY;
};

/* Ranked-game eligibility shape: exactly two teams with equal sizes
 * of 1/2/3 connected humans (1v1, 2v2, 3v3). Used by the Ranked-game
 * checkbox tooltip AND by the Ready button (Ready is disabled when
 * the lobby is flagged Ranked but the current shape doesn't qualify,
 * so the host can keep Ranked on for the games-list filter even
 * while shuffling players around). Returns the breakdown so callers
 * can show the same tooltip text. */
struct LobbyRankedEligibility {
    bool sizesEligible;
    int  teamsInUse;
    int  firstSize;
    int  secondSize;
};

/* Icon and tank textures the lobby draws, cached for the process rather
 * than the session: SDL_Texture belongs to the renderer that made it, so
 * the cache is keyed on the renderer and reloaded when that pointer
 * changes, not cleared on lobby teardown. */
typedef struct LobbyIconCache {
    SDL_Texture  *success;
    SDL_Texture  *error;
    SDL_Texture  *info;
    SDL_Texture  *settings;
    SDL_Texture  *botCpuGreen;
    SDL_Texture  *botCpuRed;
    SDL_Texture  *locked;
    SDL_Texture  *skull;
    SDL_Texture  *picture;
    SDL_Texture  *play;
    SDL_Texture  *pause;
    bool          attempted;
    /* The renderer instance the icons above were created against. SDL_Texture
     * is tied to the renderer that created it, so if the renderer instance
     * pointer changes between calls (e.g. across a game→lobby transition
     * that recreates the renderer) the cached textures reference dead GPU
     * resources. Track it and reload on mismatch — same pattern as
     * imgui_mapchooser's loadViewModeIconsOnce. */
    SDL_Renderer *renderer;

    /* Tank sprite used as the team identity badge in the lobby header.
     * Loaded once on first lobby render; tinted with the team color via
     * a darkened semi-transparent overlay. */
    SDL_Texture  *tankSelf04;
    SDL_Texture  *tankEvil04;
    SDL_Texture  *tankGood04;
    bool          tankSelfAttempted;
    bool          tankEvilAttempted;
    bool          tankGoodAttempted;
} LobbyIconCache;

/* Map-preview state: the stashed compressed map bytes plus the per-start
 * cache derived from them. */
typedef struct LobbyMapPreviewState {
    /* Compressed map data — stashed when map download completes so the popup
     * can decompress on demand (transportUdpClientGetMapData() is only called
     * in the one-shot preview-build block; the pointer may not remain valid). */
    BYTE       *popupCompressedData = NULL;
    int         popupCompressedLen  = 0;

    /* Cached compass octant (an STR_COMPASS_* lang id, 0 = unknown) per map
     * start, indexed 1-based by startIdx. MAX_STARTS is 16, so [17] covers
     * indices 1..16. Rebuilt only when the lobby map bytes change (see
     * lobbyRebuildStartCompassCache), so the player-list column never decompresses
     * the map per frame. */
    int         startCompassId[MAX_STARTS + 1] = {0};

    /* Cached start map-square positions (1-based, parallel to startCompassId)
     * plus the start bounding box and count, for the ownership-marker overlay
     * on the map previews. Rebuilt alongside the compass cache on map change so
     * the overlay never decompresses the map per frame. */
    BYTE        startMapX[MAX_STARTS + 1] = {0};
    BYTE        startMapY[MAX_STARTS + 1] = {0};
    int         startBboxMinX = 0, startBboxMinY = 0;
    int         startBboxMaxX = 0, startBboxMaxY = 0;
    BYTE        startCount = 0;

    /* 1-based start currently hovered in a start dropdown (the combo in the
     * player list), so the inline preview can outline it. Set while a dropdown
     * entry is hovered; consumed (cleared) by the preview overlay each frame. */
    int         hoveredStartChoice = -1;
} LobbyMapPreviewState;

/* Everything here belongs to one lobby session — the window's
 * visibility, its focus edge-trigger, the pending-action flags and the
 * cached ClientSim — so lobbyChooserReset() clears it on teardown. The
 * browsers themselves live in LobbyChooserTabs below and are kept. */
typedef struct LobbyChooserState {
    bool       open             = false;
    /* Edge-trigger: SetNextWindowFocus the chooser on the frame it opens
     * so it draws above the scrim windows, but NOT every frame after —
     * that yanks focus away from anything the user clicks into the
     * chat-hole. */
    bool       focusedOnce      = false;
    /* The map that was active when the window opened, so Cancel can
     * restore it (no undo packet is wired yet — the field is reserved
     * for that future work). */
    char       prevName[128]    = "";
    /* Source tab the trigger/shoulder tab-cycle wants selected next frame
     * in the map chooser, or -1 for "no forced selection". Applied via
     * ImGuiTabItemFlags_SetSelected, then cleared once after the tab bar. */
    int        forceTab         = -1;
    /* When true, the chooser window is force-sized to almost the full
     * lobby window — leaving a few chat lines visible at the bottom.
     * Toggled by the corner icon button, or by pressing Esc while the
     * chooser window has focus. */
    bool       maximized        = false;
    /* True when the user has fired at least one live-preview from a tab
     * since opening the chooser (file pick, upload kick, generate config
     * change). Drives the close-confirmation modal: dismissing the
     * window with the X (or Esc) while pending opens a "Use This Map /
     * Cancel / Keep Picking" prompt instead of silently reverting. */
    bool       previewPending   = false;
    bool       wantCloseConfirm = false;
    /* Cached ClientSim pointer for the chooser. Captured by
     * lobbyChooseMapOpen so the listProvider (which only gets a void*
     * ctx) can reach into the cs's lobbyMapList* state without each
     * call site rethreading the pointer. */
    ClientSim *cs               = NULL;
} LobbyChooserState;

#if !BOLO_MOBILE
/* Everything the reel holds belongs to one round's recap — what is playing,
 * how it is being scrubbed, the optional crop frame over it, where a round
 * still on its way from the server stands, the slack the body hands back and
 * the zoom gesture banks — so lobbyReelEnd clears the lot. */
typedef struct LobbyReelState {
    bool  active     = false;
    bool  tried      = false;  /* one load attempt per summary */
    /* The one attempt came to nothing — the viewer refused this summary's
     * bytes, or there were none to hand it. There is nothing further to try
     * for this round, so the recap says so instead of going back round for
     * more bytes. */
    bool  loadFailed = false;
    bool  drawn      = false;  /* body drew the reel this frame */
    /* True when the pause was ours (the recap stopped being drawn), not the
     * player's — the reel resumes on its own when the recap comes back, but
     * only then. */
    bool  autoPaused = false;
    float viewW      = 0.0f;
    float viewH      = 0.0f;
    /* Where the seek slider sits, and whether the player is dragging it. Held
     * apart from the playhead so a drag is not fought by the reel advancing
     * under it. The reel follows the handle live while it is dragged — see the
     * scrub block in the transport row for what each direction costs. */
    float  seekRatio      = 0.0f;
    bool   seeking        = false;
    /* Last ratio actually handed to the decoder, and when. Tracks the playhead
     * while idle so a drag starts from the truth. */
    float  seekApplied    = 0.0f;
    Uint64 seekAppliedMs  = 0;
    /* Playing state latched when a drag began, restored when it ends: a scrub
     * pauses the reel for its duration rather than letting every applied seek
     * tear down and rebuild the two SDL playback timers. */
    bool   seekWasPlaying = false;

#if BOLO_RECAP_CLIP_GIF
    /* Crop frame: an optional rectangle over the reel that an export takes
     * instead of the whole visible view, so a clip can be posted without the
     * map around it. Off by default, and off is the untouched full-view path.
     *
     * Held normalized to the displayed image rather than in pixels, so resizing
     * the lobby or zooming the reel keeps the same framing rather than leaving
     * the box pointing at a different part of the map. Session-only, by design
     * — a crop is chosen for the clip being taken, not kept as a preference,
     * which is what the reset below makes true. */
    bool  cropOn = false;
    float cropX0 = 0.25f;
    float cropY0 = 0.25f;
    float cropX1 = 0.75f;
    float cropY1 = 0.75f;

    /* The slice the reel last blitted, in render-target pixels: origin plus the
     * whole-pixel visible extent the draw computed. The crop frame is
     * normalized against this extent, so a capture can map the frame back onto
     * exactly the pixels the outline was drawn over rather than re-deriving the
     * mapping and risking a different answer. */
    SDL_Rect lastSlice = { 0, 0, 0, 0 };
#endif

    /* Where a client that did not record the round stands in getting it from
     * the server that did: the transfer's state as a ClientRoundLogState, the
     * percent that goes with it while bytes are arriving, and the latch that
     * keeps the request to one send per summary. Read by the recap so it can
     * say what is happening in place of a reel it has no bytes for yet. A
     * process replaying its own recording never asks, and leaves these idle. */
    bool    logAsked   = false;
    int     logState   = CLIENT_ROUND_LOG_IDLE;
    uint8_t logPercent = 0;
    /* Last state and ten-percent step written to winbolo.log, so the transfer
     * is traced as it moves instead of once a frame. */
    int     logStateSeen = -1;
    int     logStepSeen  = -1;

    /* Vertical room the recap left unused on the previous frame, accumulated.
     * The reel adds it to its own height, which is what stops the body ending
     * well short of the bottom of a tall panel. Immediate mode gives no way to
     * know what the content below the reel will cost before drawing it, so this
     * is a one-frame feedback loop: lobbyRenderLastRoundBody measures the shortfall
     * at the end of the frame and this grows or shrinks by that much. It has to
     * accumulate rather than hold the raw shortfall — a raw value would be
     * spent, measure zero, and collapse back the next frame. */
    float recapSlack = 0.0f;

    /* Banked wheel and pinch travel, and the frame they were last spent on, so
     * a gesture is stepped a whole notch at a time and a bank left over from a
     * gesture aimed elsewhere is dropped. See the zoom-input block below for
     * what the two units are and why the banks exist at all. */
    float wheelAccum = 0.0f;
    float pinchAccum = 0.0f;
    int   zoomFrame  = -1;
} LobbyReelState;
#endif

/* ── Functions called across a cluster boundary ───────────────────
 * Grouped by the cluster that defines them. A group's guard matches the
 * conditional its definitions sit inside, so a declaration is never visible
 * on a target that leaves the definition out.
 *
 * Ordinary C++ functions, not extern "C" — the definitions are C++ and
 * changing their linkage would change their mangling. */

/* commands */
void lobbyCommandReset(void);
void lobbySendReadyToggle(ClientSim *cs, bool ready);
const LobbyBrainMeta *lobbyBrainMetaFor(const char *name);
void lobbyDrawTagline(const char *tag, float wrapPosX);
void lobbyGearTooltip(ClientSim *cs, int slot, float s);
bool lobbyAddBotPending(ClientSim *cs);
void lobbySendAddBotDebounced(ClientSim *cs,
                              int namingPool, uint8_t teamNumber);
void lobbySendRemoveBot(ClientSim *cs, uint8_t slot);
void lobbySendTeamSet(ClientSim *cs,
                      uint8_t targetSlot, uint8_t teamNumber);
void lobbySendBotConfig(ClientSim *cs,
                        uint8_t slot,
                        uint8_t difficulty, uint8_t personality,
                        const char *name);
void lobbySendSetBotBrain(ClientSim *cs,
                          uint8_t slot, uint8_t brainIdx);
void lobbySendTeamClear(ClientSim *cs, uint8_t teamId);
void lobbySendTeamPool(ClientSim *cs,
                       uint8_t teamId, uint8_t namingPool,
                       const char *teamName);
void lobbySendSetting(ClientSim *cs,
                      uint8_t settingType,
                      const uint8_t *value, uint8_t valueLen);

/* chooser */
void lobbyChooserReset(void);
void lobbyChooseMapOpen(ClientSim *cs, SDL_Renderer *renderer);
void lobbyChooseMapRenderWindow(ClientSim *cs, SDL_Renderer *renderer,
                                float s, int screenW, int screenH);

/* chat */
void lobbyChatReset(void);
void lobbyRenderChatHistory(const char *blob);
void lobbyRenderChatInputAndSend(ClientSim *cs, char *chatInput,
                                 BYTE myPlayerNum, bool hasTransport,
                                 float s, BYTE destPlayer);
#if !BOLO_MOBILE
void lobbyChatInputAppendTime(uint32_t curMs);
#endif

/* wbnmaps */
#ifndef __EMSCRIPTEN__
void lobbySpWbnReset(void);
void lobbyWbnMapsListProvider(MapChooserState *state,
                              const char *relPath, void *ctx);
void lobbyWbnMapsOnSelect(MapChooserState *state, void *ctx);
void lobbyWbnMapsOnFolderJump(MapChooserState *state,
                              const char *jumpPath, void *ctx);
bool lobbyWbnGeneratePreview(const char *entryPath,
                             MapPreviewPixels *outBuf,
                             void *ctx);
void lobbyWbnMapsTick(MapChooserState *state, SDL_Renderer *renderer,
                      void *ctx);
void lobbyWbnMapsTooltipPrefix(MapChooserState *state, void *ctx);
#endif

/* mappreview */
void lobbyMapPreviewReset(void);
void lobbyRebuildStartCompassCache(const BYTE *data, int len);
const char *lobbyMapTransferLine(ClientSim *cs, float *outProgress);
int lobbyComputeStartOwners(ClientSim *cs, int myPlayerNum,
                            uint8_t *owners, int maxN, uint32_t *outSig);
void lobbyDrawPreviewStartOverlay(ClientSim *cs, int myPlayerNum,
                                  ImVec2 imgMin, float previewSize,
                                  int bx0, int by0, int bx1, int by1);
bool lobbyPreviewInteract(ClientSim *cs, int myPlayerNum, bool effHostMap,
                          ImVec2 imgMin, float innerSize,
                          int bx0, int by0, int bx1, int by1);
SDL_Texture *lobbyBuildMapPreview(SDL_Renderer *renderer,
                                  const BYTE *compressedData, int dataLen,
                                  LobbyMapBounds *bounds,
                                  const uint8_t *startOwners, int ownerCount);

/* assets */
const char *lobbyGameTypeStr(gameType gt);
const char *lobbyAiTypeStr(uint8_t ai);
void lobbyFormatTimeLimit(int32_t ticks, char *buf, int bufSize);
SDL_Texture *lobbyGetTankSelf04Texture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetTankEvil04Texture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetTankGood04Texture(SDL_Renderer *renderer);
void lobbyLoadStatusIconsOnce(SDL_Renderer *renderer, float scale);

/* players */
void lobbyPlayersReset(void);
LobbyRankedEligibility lobbyComputeRankedEligibility(ClientSim *cs);
void lobbyRankedShapeTooltip(const LobbyRankedEligibility &r);
bool lobbyIsHost(ClientSim *cs, int myPlayerNum);
void lobbyRenderAllowNewPlayersRow(ClientSim *cs,
                                   int myPlayerNum, float s);
void lobbyRecapRowJump(ClientSim *cs, int slot, bool isBot);
void lobbyRenderTeamGroupedPlayers(ClientSim *cs,
                                   int myPlayerNum, float s, bool isHost);

/* status */
void lobbyRenderConnectivityBadge(SDL_Renderer *renderer, float s);
void lobbyRenderRejectToast(ClientSim *cs, float s);

/* recap */
void lobbyRecapReset(void);
bool lobbyRecapReelVisible(ClientSim *cs);
void lobbyRenderLastRoundBody(ClientSim *cs, float s);
void lobbyRenderLockBadge(void);
void lobbyRenderMapSkipVote(ClientSim *cs, bool spectator, bool hasTransport,
                            float s, bool sameLine);

/* reel */
#if !BOLO_MOBILE
void lobbyReelEnd(void);
void lobbyReelDropRoundLog(ClientSim *cs);
void lobbyRenderReel(ClientSim *cs, const RoundStatsSummary *st,
                     float s);
#endif

/* clipgif */
#if !BOLO_MOBILE && BOLO_RECAP_CLIP_GIF
void lobbyClipGifReset(void);
bool lobbyClipGifActive(void);
void lobbyClipGifStartClip(const HighlightWindow *h, const char *mapName);
void lobbyClipGifStartFromPlayhead(uint32_t curMs, const char *mapName);
bool lobbyClipGifButton(const char *id, bool compact);
float lobbyClipGifButtonWidth(void);
void lobbyClipGifRender(float s);
#endif

/* rating */
#if !BOLO_MOBILE && BOLO_RECAP_WBN_RATING
void lobbyRatingReset(void);
void lobbyRatingSyncKey(ClientSim *cs, const RoundStatsSummary *st);
void lobbyRenderRatingBlock(ClientSim *cs, const RoundStatsSummary *st,
                            float s);
#endif

/* settings */
void lobbySettingsPostGameEdge(bool showLastRound);
void lobbyRenderGameSettingsPanel(ClientSim *cs,
                                  int myPlayerNum, float s);
void lobbyRenderGameSettingsBody(ClientSim *cs, int myPlayerNum, float s);

/* ── State accessors ──────────────────────────────────────────────
 * The lobby's state lives in per-cluster structs, each private to the source
 * that owns it. These are the only way another cluster reaches into one.
 *
 * An instance with three or more fields read from outside hands back the
 * whole struct; one or two fields hand back just those. Every accessor below
 * returns a pointer, because in each case the foreign cluster writes the
 * field, or the field is an array or an aggregate. */

/* s_icons, owned by assets. Read by players, status, recap, reel and clipgif
 * for the icon and tank textures. */
LobbyIconCache       *lobbyIcons(void);
/* s_mapPreview, owned by mappreview. Read by core and players for the stashed
 * map bytes and the per-start caches. */
LobbyMapPreviewState *lobbyMapPreview(void);
/* s_chooser, owned by chooser. Read by core for the window's visibility and
 * focus edge, and by wbnmaps to flag a fired preview. */
LobbyChooserState    *lobbyChooser(void);
#if !BOLO_MOBILE
/* s_reel, owned by reel. Read by core for what is playing, by recap for the
 * slack it hands back, and by clipgif for the crop frame and last slice. */
LobbyReelState       *lobbyReel(void);
#endif

/* s_chat.blockMin / .blockMax, owned by chat. Core writes the chat block's
 * screen rect each frame and the chooser scrim reads it back. */
ImVec2   *lobbyChatBlockMin(void);
ImVec2   *lobbyChatBlockMax(void);
/* s_commands.botNameOverridden, owned by commands. Players sets and clears
 * the per-slot flag; indexed by lobby slot. */
bool     *lobbyCommandBotNameOverridden(void);
/* s_brains.lastChosenBrainIdx, owned by commands. Players writes the host's
 * sticky brain pick. 0xFF means "no sticky yet". */
uint8_t  *lobbyBrainLastChosenIdx(void);
/* s_players.forceTab, owned by players. Core sets it from the shoulder
 * tab-cycle and clears it after the tab bar. -1 means "no forced selection".
 * Not to be confused with LobbyChooserState::forceTab, which is
 * chooser-internal. */
int      *lobbyPlayersForceTab(void);
/* s_recap.showMap, owned by recap. Core reads it to pick the right-hand panel
 * and flips it from the panel's own button. */
bool     *lobbyRecapShowMap(void);
#ifndef __EMSCRIPTEN__
/* s_chooserTabs.wbn, owned by chooser. wbnmaps drives the WinBolo.net tab's
 * browser through it. */
MapChooserState *lobbyChooserWbnTab(void);
#endif /* __EMSCRIPTEN__ */
/* s_lf.chatInput, owned by core. Chat's timestamp helper appends into it;
 * LOBBY_CHAT_INPUT_SIZE bytes. */
char     *lobbyFrameChatInput(void);

#ifdef __cplusplus
extern "C" {
#endif

/* Embedded log-viewer reel (src/logviewer/lv_embed.c). Hand-declared rather
 * than included: logviewer.h pulls in backend.h / viewport_types.h, whose
 * screen / screenMines types collide with the client's — the same rule
 * gamefront.c documents. Scalars and void * only, so no viewer type crosses
 * the seam. Must stay inside this extern "C" block or the calls compile and
 * then fail to link on a mangled symbol. */
#if !BOLO_MOBILE
bool lvEmbedBegin(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  uint8_t *zipData, size_t zipLen, int viewW, int viewH);
void lvEmbedEnd(void);
bool lvEmbedIsActive(void);
void lvEmbedSetViewportSize(int viewW, int viewH);
bool lvEmbedFrameTexture(void **outTexture, int *outTexW, int *outTexH,
                         int *outSrcX, int *outSrcY, int *outSrcW, int *outSrcH);
float lvEmbedGetZoomLevel(void);
void lvEmbedPlay(void);
void lvEmbedPause(void);
bool lvEmbedIsPlaying(void);
void lvEmbedWheel(int localX, int localY, float wheelY);
void lvEmbedPanBegin(void);
void lvEmbedPanDelta(float dxScreenPx, float dyScreenPx);
void lvEmbedGetProgress(uint32_t *outCurMs, uint32_t *outTotalMs);
void lvEmbedSeekRatio(float ratio);
void lvEmbedSeekToClip(uint32_t roundRelMs, int mapX, int mapY);
void lvEmbedSeekToTime(uint32_t roundRelMs);
bool lvEmbedFocusPlayerByName(const char *name);
void lvEmbedStepTicks(int ticks);
void lvEmbedSetSelfName(const char *name);
#endif
#if !BOLO_REEL_WBN_FETCH_CURL
/* Round-log download for the reel's WinBolo.net source
 * (src/wasm/round_log_fetch_wasm.c). Start hands the request to the page and
 * returns; the poll is read once a frame, so nothing here suspends the C stack
 * inside an ImGui frame. Poll gives 0 while the request is in flight, otherwise
 * the HTTP status, or -1 when it never completed; on a 200 whose body fits the
 * ceiling it hands over a malloc'd buffer the caller owns. Hand-declared on the
 * same terms as the reel above. */
void wbRoundLogFetchStart(const char *key);
int  wbRoundLogFetchPoll(uint8_t **outBuf, int *outLen);
void wbRoundLogFetchCancel(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LOBBY_INTERNAL_H */
