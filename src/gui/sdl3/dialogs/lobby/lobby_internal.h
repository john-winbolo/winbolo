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
 *   sources, and the save path ends in a native file dialog the browser has
 *   no equivalent of. */
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
#include "brain_list.h"      /* BrainModes — cached per catalogue entry below */
#include "types.h"
#include "imgui_mapchooser.h"
/* VisibilitySettings, named by the value renderer the server browser and
 * the in-game info panel share with the lobby. */
#include "../../../visibility_presets.h"
#ifdef __cplusplus
}
#endif

/* Sizes shared between the cluster that owns the widget and the ones that
 * measure against it. The player row's badge run used to be here as a flat
 * 14 px; it is sdl3ImguiWbnIconPx() now (sdl3imgui.h), so it comes out the
 * same height as the cog beside it at every UI scale. */
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
    uint32_t color;        /* 0xRRGGBB the bot's name tag is painted with */
    /* The brain's own modes.txt, parsed once per catalogue entry: what the
     * gear popup's Mode dropdown lists and where the Difficulty dropdown's
     * entries come from. Always populated — a brain with no manifest gets
     * the synthesized single "default" mode (brain_list.h). */
    BrainModes modes;
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
    /* The same chip with every colour taken to grey — the "off" chip in the
     * player list's difficulty tag, where a lit chip is botCpuRed. A real
     * greyscale asset rather than the red one drawn faded, so an off chip
     * keeps the art's full contrast instead of washing out against the pill. */
    SDL_Texture  *botCpuGrey;
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

    /* Pillbox and base sprites for the read-only view-policy summary,
     * which pairs each with the tank sprite above. Loaded lazily by
     * their getters and reloaded on a renderer swap, same as the tanks. */
    SDL_Texture  *pillbox15;
    SDL_Texture  *baseGood;
    bool          pillbox15Attempted;
    bool          baseGoodAttempted;

    /* Forest tile, drawn under a shrunken ally tank as the allies-in-trees
     * entry on the same summary. Lazily loaded and reloaded like the rest. */
    SDL_Texture  *forest;
    bool          forestAttempted;

    /* The default ping marker, for the header summary's smart-ping entry.
     * Its own copy rather than the game renderer's ping_icons.c cache: that
     * one single-owns a renderer and reloads the whole set whenever the
     * pointer changes, so sharing it would make the lobby and the game
     * thrash it across every screen swap. Lazily loaded and destroyed on a
     * renderer swap like the rest of this cache. */
    SDL_Texture  *pingStandard;
    bool          pingStandardAttempted;
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
    /* How many of those starts are on the map. A start the map loader took
     * off (one in the mined border) keeps its slot with a compass id of 0. */
    BYTE        startLiveCount = 0;
    /* Slot and live counts of the pillboxes and bases on the same map, for
     * the "P / B" counts the lobby shows. The loader takes one in the mined
     * border off the map, the same as a start. */
    BYTE        pillCount = 0;
    BYTE        pillLiveCount = 0;
    BYTE        baseCount = 0;
    BYTE        baseLiveCount = 0;

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

/* The three colours (bg / text / border) a bot's NAME tag is drawn with on
 * the player list, from the brain's own colour: the one its about.txt
 * declares, else the one remembered in the prefs for that name, else one
 * derived from the name and remembered from then on. */
void lobbyBotBrainTagColors(ClientSim *cs, int slot,
                            ImU32 *bg, ImU32 *fg, ImU32 *border);
void lobbyDrawTagline(const char *tag, float wrapPosX, int difficulty);
void lobbyGearTooltip(ClientSim *cs, int slot, float s);
/* Per-difficulty wording (lang ids) and the bot's brain name without its
 * version suffix. Difficulty is a BOT_DIFFICULTY_* value; anything out of
 * range reads as Hard. */
unsigned int lobbyBotDifficultyLabelId(uint8_t difficulty);
unsigned int lobbyBotDifficultyTaglineId(uint8_t difficulty);
unsigned int lobbyBotDifficultyDescId(uint8_t difficulty);
void lobbyBotBrainBaseName(ClientSim *cs, int slot, char *out, size_t outSz);
bool lobbyAddBotPending(ClientSim *cs);
void lobbySendAddBotDebounced(ClientSim *cs,
                              int namingPool, uint8_t teamNumber);
void lobbySendRemoveBot(ClientSim *cs, uint8_t slot);
void lobbySendTeamSet(ClientSim *cs,
                      uint8_t targetSlot, uint8_t teamNumber);
/* mode indexes the brain's own mode list, difficulty that mode's level
 * list — see brain_list.h and lobbyBotModesFor below. */
void lobbySendBotConfig(ClientSim *cs,
                        uint8_t slot,
                        uint8_t mode, uint8_t difficulty, uint8_t personality,
                        const char *name);

/* The mode manifest of the brain a lobby slot is running, cached per
 * catalogue entry. Never NULL for a slot whose brain list has arrived; NULL
 * only when there is no catalogue yet (a client mid-join). */
const BrainModes *lobbyBotModesFor(ClientSim *cs, int slot);

/* This slot's mode / level indices, clamped against the brain's actual
 * manifest so a stale byte can never index past the list. Either out
 * pointer may be NULL. */
void lobbyBotModeAndLevel(ClientSim *cs, int slot,
                          int *outMode, int *outLevel);

/* True for the brain's default mode — the one every ordinary game uses,
 * which the row and the gear tooltip leave unnamed. */
bool lobbyBotModeIsDefault(const BrainModes *modes, int mode);

/* True when that mode's levels are exactly easy / medium / hard and the
 * mode is the default one or declares `standard_levels = yes` in modes.txt
 * (brainModeUsesStandardLevels), so the STR_BOT_DIFF_* wording actually
 * describes them. False for any other mode — those show the manifest's own
 * labels. True when there is no catalogue yet. */
bool lobbyBotModeUsesLangLevels(const BrainModes *modes, int mode);
void lobbySendSetBotBrain(ClientSim *cs,
                          uint8_t slot, uint8_t brainIdx);
void lobbySendTeamClear(ClientSim *cs, uint8_t teamId);
void lobbySendTeamPool(ClientSim *cs,
                       uint8_t teamId, uint8_t namingPool,
                       const char *teamName);
void lobbySendTeamSide(ClientSim *cs, uint8_t teamId, uint8_t startSide);
void lobbySendSetting(ClientSim *cs,
                      uint8_t settingType,
                      const uint8_t *value, uint8_t valueLen);

/* chooser */
void lobbyChooserReset(void);
void lobbyChooseMapOpen(ClientSim *cs, SDL_Renderer *renderer);
void lobbyChooseMapRenderWindow(ClientSim *cs, SDL_Renderer *renderer,
                                float s, int screenW, int screenH);

/* scenariochooser — the dialog behind the two Details buttons on the lobby's
 * script lines: what the server offers on its own, and the round's own list.
 *
 * Open is what either button calls — the mods row's, in the host-only Server
 * Settings column, and the map panel's, which everybody is shown. The dialog
 * asks lobbyScenarioMayChoose whether this client may reorder the list and
 * draws itself read-only when it may not, which is what lets the second
 * button exist at all.
 *
 * The window is drawn from the lobby's own frame, beside the map chooser's,
 * rather than from either line that opens it: both lines are drawn inside
 * something that can stop being drawn, and a dialog that stopped being drawn
 * when the player changed tab would be open with no way back to it. IsOpen is
 * what the lobby reads so its Esc and its controller tab cycle stand aside
 * while the dialog is up. */
void lobbyScenarioChooserReset(void);
void lobbyScenarioChooserOpen(void);
bool lobbyScenarioChooserIsOpen(void);
void lobbyScenarioChooserRenderWindow(ClientSim *cs, float s,
                                      int screenW, int screenH);

/* scenariodetails — what one scenario or mod is, behind every script name
 * the lobby draws as a link: the map panel's lines, the Server Settings
 * column's row and every row of the chooser.
 * One dialog for all three, because the question is the same one.
 *
 * OpenScript describes one row of the lobby's ordered script list, which is
 * what the lobby lines are drawn from: each name opens its own row, so the
 * names on a round running several scripts each describe their own.
 * OpenAttached is the same dialog for the one attached script, for a client
 * whose server has sent no list. The chooser's rows open it on a listing
 * entry instead, which it snapshots. The modal must be rendered at the lobby
 * window's own id scope, the way the bot docs dialog is: none of the buttons
 * that open it is at that scope, so the ask is a flag and this call is what
 * turns it into an OpenPopup.
 *
 * IsOpen is what the lobby reads so its own Escape stands aside. */
void lobbyScenarioDetailsReset(void);
void lobbyScenarioDetailsOpenAttached(ClientSim *cs);
void lobbyScenarioDetailsOpenScript(ClientSim *cs, int idx);

/* Which row of that list is the scenario, and which the first mod. -1 for
 * none of that kind. Lives beside the two renderers that classify the list,
 * in lobby_assets.cpp, so the lines and the links agree about which row is
 * which. */
int  lobbyScriptRowIndexOfKind(ClientSim *cs, bool mod);
bool lobbyScenarioDetailsIsOpen(void);
/* s is the lobby's own dialog scale, needed because the header's kind tag is
 * the same chip the chooser's rows draw and that chip is sized in scaled
 * pixels. The modal is rendered from the lobby's frame, which is where that
 * scale is, so it is handed down rather than worked out again here. */
void lobbyScenarioDetailsRenderModal(ClientSim *cs, float s);

/* chat */
void lobbyChatReset(void);
void lobbyRenderChatHistory(const char *blob);
/* A bot's announce line in team chat, and the docs dialog behind it. The
 * announce text is registered as the exact block that was appended to the
 * chat blob; lobbyRenderChatHistory matches it back out and draws it as a
 * link. The modal must be rendered at the lobby window's own id scope. */
/* Longest announce block the chat can carry: a bot name, ": ", and the
 * brain's whole announce.txt (BRAIN_ANNOUNCE_MAX). */
#define LOBBY_CHAT_DOCS_LINE_MAX 640
void lobbyChatDocsReset(void);
int  lobbyChatDocsCount(void);
void lobbyChatDocsRegister(int brainIdx, const char *brainName,
                           const char *text);
void lobbyChatDocsRenderModal(ClientSim *cs);
void lobbyRenderChatInputAndSend(ClientSim *cs, char *chatInput,
                                 BYTE myPlayerNum, bool hasTransport,
                                 float s, BYTE destPlayer);
#if !BOLO_MOBILE
void lobbyChatInputAppendTime(uint32_t curMs);
#endif

/* wbnmaps — the sim-facing half of the WinBolo.net tab's provider; the
 * catalogue callbacks come from wbn_map_source.h. */
#ifndef __EMSCRIPTEN__
void lobbyWbnMapsOnSelect(MapChooserState *state, void *ctx);
void lobbyWbnMapsTick(MapChooserState *state, SDL_Renderer *renderer,
                      void *ctx);
#endif

/* mappreview */
void lobbyMapPreviewReset(void);
void lobbyRebuildStartCompassCache(const BYTE *data, int len);
/* Side mask of 1-based start k from the per-start cache; 0 (centre) when k
 * is off the cached list. */
BYTE lobbyStartSideMask(int k);
/* Starts on the lobby map, for the start counts the lobby shows: the cache's
 * live count once it holds this map, otherwise the server's slot count. */
int lobbyLiveStartCount(ClientSim *cs);
/* The same for pillboxes and bases. */
int lobbyLivePillCount(ClientSim *cs);
int lobbyLiveBaseCount(ClientSim *cs);
const char *lobbyMapTransferLine(ClientSim *cs, float *outProgress);
int lobbyComputeStartOwners(ClientSim *cs, int myPlayerNum,
                            uint8_t *owners, int maxN, uint32_t *outSig);
void lobbyDrawPreviewStartOverlay(ClientSim *cs, int myPlayerNum,
                                  ImVec2 imgMin, float previewSize,
                                  int bx0, int by0, int bx1, int by1);
/* The two-team N/S · E/W compass rose in the preview's bottom-left corner.
 * Host only, map with starts, exactly two teams with members. Returns true
 * when the cursor is on it, so the caller skips the start claim/drag layer
 * and the zoom popup. Call it before lobbyPreviewInteract. */
/* Hit test only, run BEFORE the start claim/drag layer so the rose keeps
 * mouse priority: true while the cursor is within the rose's reach. */
bool lobbyPreviewCompassHot(ClientSim *cs, bool effHostMap,
                            ImVec2 imgMin, float innerSize, float gapPx, float s);
/* Draw + tooltip + click, run AFTER the start overlay so the letters paint
 * on top of any start label pushed into the corner. */
bool lobbyDrawPreviewCompass(ClientSim *cs, bool effHostMap,
                             ImVec2 imgMin, float innerSize, float gapPx, float s);
bool lobbyPreviewInteract(ClientSim *cs, int myPlayerNum, bool effHostMap,
                          ImVec2 imgMin, float innerSize,
                          int bx0, int by0, int bx1, int by1);
SDL_Texture *lobbyBuildMapPreview(SDL_Renderer *renderer,
                                  const BYTE *compressedData, int dataLen,
                                  LobbyMapBounds *bounds,
                                  const uint8_t *startOwners, int ownerCount);

/* assets */
/* One name tag: a square chip with a themed fill, an optional 1px border and
 * its label at 70% of the font size. The player list wears these beside a
 * name as HOST / ADMIN / BOT and the Mods dialog wears them on every row as
 * Mod / Scenario, so the geometry lives here and neither of them owns a copy
 * of it.
 *
 * It places nothing of its own. The caller does its SameLine and its vertical
 * nudge first, because the two callers centre the chip on different things —
 * a player row on the row's midline, a Mods row on the name beside it — and
 * a SameLine inside here would undo whichever of the two ran before it. The
 * chip is drawn at the cursor and its space booked with a Dummy, so whatever
 * follows starts after it. A border of 0 draws none. s is the caller's own
 * dialog scale, the same one every other `X * s` in that dialog uses.
 *
 * The two measurements are that same geometry read without drawing it, for a
 * caller that has to lay out the room around the chip before either is
 * drawn. */
void  lobbyDrawNameTag(const char *label, ImU32 bg, ImU32 text, ImU32 border,
                       float s);
float lobbyNameTagWidth(const char *label, float s);
float lobbyNameTagHeight(float s);
/* That chip filled in with the one word that says which of the two kinds a
 * script is — Mod or Scenario, in the two colours the theme keeps for them.
 * Worn by every row of the chooser, by the details dialog's header and by
 * the map panel's script links, so it lives here rather than in whichever of
 * the three was written first.
 *
 * It puts its own SameLine in front of itself and centres on the item it
 * follows, so the caller draws the name and then calls this with nothing in
 * between. Width is the same geometry read without drawing it, the leading
 * spacing included, for a caller laying out the room around the chip first —
 * the chooser's rows measure the name against it, and the map panel's mod
 * links measure the wrap point against it. */
void  lobbyScenarioKindTag(bool mod, float s);
float lobbyScenarioKindTagWidth(bool mod, float s);
/* The "Workshop" chip a script published to the Steam Workshop wears after
 * its kind chip, and the "Open in Workshop" button that opens its page. Each
 * draws nothing and measures 0 for a Workshop id of 0, and the button also
 * for a build or a run where Steam's Workshop is not available, so a caller
 * hands them the row's id and draws and measures them on every row.
 *
 * The chip is placed as the kind chip is: a SameLine of its own, centred on
 * the item before it, which is the kind chip. The button puts its own
 * SameLine in front of itself. Both widths count the spacing in front. */
void  lobbyScenarioWorkshopTag(uint64_t workshopId, float s);
float lobbyScenarioWorkshopTagWidth(uint64_t workshopId, float s);
void  lobbyScenarioWorkshopLink(uint64_t workshopId);
float lobbyScenarioWorkshopLinkWidth(uint64_t workshopId);
const char *lobbyGameTypeStr(gameType gt);
/* What is playing, in two shapes for the two places that ask.
 *
 * Line is the settings form's Server Settings column: the Mods/Scenario row,
 * which names every script with its Mod or Scenario tag. The row is drawn
 * even at none, because this is where the control to change them lives. For
 * a host who has the scripts preference switched off, one line saying so
 * and no row.
 *
 * effectiveHost is the caller's own answer to whether this viewer may edit
 * lobby state, handed down rather than worked out again here: the mods row
 * holds a real setting and its checkbox has to be disabled on the same test
 * as the time-limit and password rows beside it in that column.
 *
 * InfoLines is the map panel, for everyone: the same two facts read-only,
 * each line dropped where it has nothing to name, the scenario's name and
 * each mod's name a link that opens the details dialog, and a Details button
 * under them that opens the chooser. That button is the only way into the
 * chooser a joiner or a spectator has — the column Line draws in is host-only
 * — and the chooser has always known how to draw itself read-only.
 *
 * Two renderers rather than one with a flag: they word the same facts
 * differently and disagree about the empty case on purpose. Both only ask
 * for their dialogs; the lobby's own frame is what draws them.
 *
 * s is the caller's own dialog scale, needed by both because the kind chip
 * beside each script name is sized in scaled pixels. */
void lobbyRenderScenarioLine(ClientSim *cs, bool effectiveHost, float s);
void lobbyRenderScenarioInfoLines(ClientSim *cs, float s);
/* Whether the round runs mods, for the lobby's header line: "Mods: Yes (3)"
 * or "Mods: No", with the names on the hover in the order the server
 * published them. Yes only when the setting is on and the round carries at
 * least one, since either half alone means nothing runs.
 *
 * On that line for the reason the visibility and smart-ping summaries are:
 * the checkbox and the list of names are both in the host-only settings
 * column, so this is where everybody else is told. */
void lobbyRenderModsSummary(ClientSim *cs, float s);
/* Whether this client may change which scripts play: the host slot, an admin,
 * or anybody at all while Open Host is on — and never a spectator. The same
 * answer lobbyClientMayEdit gives on the server, minus that last term, so a
 * client is never shown arrows the server would refuse and never refused
 * arrows the server would take. Shared with the chooser, which is where the
 * controls it gates actually are. */
bool lobbyScenarioMayChoose(ClientSim *cs);
const char *lobbyAiTypeStr(uint8_t ai);
void lobbyFormatTimeLimit(int32_t ticks, char *buf, int bufSize);
SDL_Texture *lobbyGetTankSelf04Texture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetTankEvil04Texture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetTankGood04Texture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetPillbox15Texture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetBaseGoodTexture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetForestTexture(SDL_Renderer *renderer);
SDL_Texture *lobbyGetPingStandardTexture(SDL_Renderer *renderer);
void lobbyLoadStatusIconsOnce(SDL_Renderer *renderer, float scale);

/* players */
void lobbyPlayersReset(void);
/* src into out, cut short with a trailing "..." where it is wider than maxW
 * pixels. A maxW of 1 or less is "no limit" and copies the lot. */
void lobbyTruncateName(const char *src, float maxW, char *out, size_t outSz);
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
/* The clip export's save picker is asynchronous. The poll writes out a clip
 * whose picker has been answered and belongs in the lobby's event loop, above
 * the view switch, so an export survives navigating away from the recap; the
 * abandon releases one still waiting when the lobby session ends. */
void lobbyClipGifSavePoll(void);
void lobbyClipGifSaveAbandon(void);
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
/* Read-only one-line summary of the three view policies. */
void lobbyRenderVisibilitySummary(ClientSim *cs, float s);
/* Read-only ping-marker-and-answer pair for the same header line. */
void lobbyRenderSmartPingSummary(ClientSim *cs, float s);

/* ── One visibility value, drawn the one way ──────────────────
 * The lobby's header line, the Details table, the server browser and the
 * in-game info panel all show the same eight settings, so they all draw a
 * value through this: the setting's sprite and the word it is on, faint
 * together when it is off, with the seconds added under Decay.
 *
 * column runs 0..LOBBY_VIS_COLUMN_COUNT-1 in the order the Details table
 * reads: pill view, base view, allied tank view, allies in trees, the
 * overview window, line of sight, positional sound. The overview window
 * and line of sight have no sprite and come back as the word alone. The
 * whole thing is one item, so the caller's IsItemHovered covers it.
 *
 * lobbyVisibilityColumnLabelId names the setting, for a caller that lays
 * out its own label — the browser's detail pane does, the header line
 * does not. */
#define LOBBY_VIS_COLUMN_COUNT 7
void lobbyRenderVisibilityColumn(const VisibilitySettings *v, int column,
                                 float s);
int  lobbyVisibilityColumnLabelId(int column);
/* The same value as words, for a caller with no room to draw a sprite —
 * a tooltip, or a line of running text. Seconds included under Decay. */
void lobbyVisibilityColumnText(const VisibilitySettings *v, int column,
                               char *out, size_t outSize);
/* A whole set on one line: "Pills Key · Bases Off · ...", short labels and
 * the same value words, with the two on/off rules named only while they
 * are on. What the lobby's Custom row is described by, and what a server
 * browser row's hover says a game running no named set is doing. */
void lobbyVisibilityDetailsLine(const VisibilitySettings *v, char *out,
                                size_t outSize);

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
int  lvEmbedTankLabelCount(float pxPerSourcePx);
bool lvEmbedTankLabel(int index, void **outTexture, int *outX, int *outY,
                      int *outW, int *outH);
void lvEmbedSetTankLabelsInTexture(bool inTexture);
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
