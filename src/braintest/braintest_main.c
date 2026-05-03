/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          BrainTest
 * Filename:      braintest_main.c
 * Purpose:
 *   Standalone brain debug viewer. Runs a ServerSim with
 *   bot(s) and renders the game with debug overlays for
 *   the brain's internal state: influence grid, danger
 *   grid, front line, and pathfinder route.
 *
 *   Usage:
 *     BrainTest [options]
 *
 *   Options:
 *     -brain PATH      Brain script (default: brains/NewAutopilot)
 *     -noplayers N     Number of bot players (default: 1)
 *     -map PATH        Map file (default: built-in Everard Island)
 *     -follow N        Follow bot N with camera (default: 0)
 *
 *   Controls:
 *     Arrow keys       Scroll the map (free camera)
 *     Space            Pause / unpause
 *     Tab              Cycle followed bot
 *     1                Toggle influence overlay
 *     2                Toggle danger overlay
 *     3                Toggle front line overlay
 *     4                Toggle path overlay
 *     0                Toggle all overlays off
 *     +/-              Zoom in/out
 *     Scroll wheel     Zoom in/out
 *     F                Toggle free camera / follow mode
 *     Left click       Show A* cost + path to clicked tile (magenta)
 *     Right click      Clear click path overlay
 *********************************************************/

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "../bolo/global.h"
#include "../bolo/everard_map.h"
#include "../bolo/tank.h"
#include "../bolo/bot_manager.h"
#include "../bolo/players.h"
#include "../bolo/bolo_map.h"
#include "../bolo/pillbox.h"
#include "../bolo/bases.h"
#include "../bolo/starts.h"
#include "../bolo/allience.h"
#include "../bolo/brain_pathfinder.h"
#include "../bolo/gui_message.h"
#include "../server/server_sim.h"
#include "../gui/sdl3/mapview.h"
#include "../gui/sdl3/tileloader.h"
#include "../gui/sdl3/luabrainshandler.h"
#include "../bolo/braincore.h"
#include "../bolo/brain_overlay.h"
#include "braintest_viz_registry.h"
#include "braintest_vizwindow.h"
#include "braintest_panel_registry.h"
#include "braintest_panelwindow.h"
#include "braintest_panel_types.h"
#include "braintest_botwindow.h"
#include "braintest_mainimgui.h"
#include "braintest_shotsimpanel.h"
#include "braintest_shotsim_poi_registry.h"
#include "braintest_vizdetail_registry.h"
#include "braintest_vizdetailwindow.h"
#include "braintest_pillcontrib_registry.h"
#include "../bolo/brain_pathfinder.h"

/* Built-in text renderer lives in panelwindow.cpp (needs ImGui). */
void panelRenderText(int registry_idx, const char *body);
#include "../gui/clientmutex.h"
#include "../gui/lang.h"

/* ------------------------------------------------------------------ */
/* Globals needed by the engine                                        */
/* ------------------------------------------------------------------ */
bool isInMenu = FALSE;

/* ------------------------------------------------------------------ */
/* gameFront* stubs                                                    */
/* ------------------------------------------------------------------ */
void gameFrontGetPassword(char *pword) { pword[0] = '\0'; }
void gameFrontGetPlayerName(char *pn) { strcpy(pn, "BrainTest"); }
void gameFrontSetPlayerName(char *pn) { (void)pn; }
void gameFrontSetAIType(aiType ait) { (void)ait; }
void gameFrontEnableRejoin(void) {}

time_t windowsGetTicks(void) { return (time_t)winboloTimer(); }
time_t serverMainGetTicks(void) { return (time_t)winboloTimer(); }

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define TICK_INTERVAL_MS   20   /* 50 Hz game tick */
#define DEFAULT_WINDOW_W  1280
#define DEFAULT_WINDOW_H   800
#define SCROLL_SPEED        4   /* map tiles per second of held arrow key */
#define MIN_ZOOM            1
#define MAX_ZOOM           16
#define CONTROL_BAR_HEIGHT 32   /* timeline scrubber strip at bottom */
#define MAX_RECORDING_FRAMES 90000

/* Playback / live-replay speed presets, ms-per-tick. Lower = faster. */
static const int SPEED_PRESETS[] = {
    1, 2, 5, 10, 20, 40, 80, 150, 300, 500, 1000, 2000, 5000
};
#define NUM_SPEED_PRESETS (int)(sizeof(SPEED_PRESETS) / sizeof(SPEED_PRESETS[0]))
#define DEFAULT_SPEED_INDEX 4  /* 20ms = real-time 50Hz */

/* ------------------------------------------------------------------ */
/* Recording infrastructure (per-tick snapshot for timeline scrubber)  */
/*                                                                     */
/* Ported from optimize-perf branch. Per-tick captures the followed    */
/* bot's view of the world: map terrain + pathfinder grids + brainMap  */
/* with delta encoding (every KEYFRAME_INTERVAL frames is a full       */
/* keyframe; the others store only changed bytes vs. the previous      */
/* frame), plus tank/shell/base/pill snapshots and the per-panel JSON  */
/* the brain emitted that tick. Playback re-applies deltas onto a      */
/* shared playback* buffer that BrainTest's existing renderers read    */
/* from when app->playbackMode is on.                                  */
/* ------------------------------------------------------------------ */

#define KEYFRAME_INTERVAL 500   /* full snapshot every N frames */

typedef struct {
    uint32_t offset;
    uint8_t  oldVal;
    uint8_t  newVal;
} ByteDelta;

typedef struct {
    uint32_t offset;
    int16_t  oldVal;
    int16_t  newVal;
} GridDelta;

typedef struct {
    uint32_t tick;
    bool     isKeyframe;

    /* Map terrain (256*256 bytes): keyframe carries fullMap, deltas
     * carry only changed bytes vs. previous frame. */
    BYTE      *fullMap;
    ByteDelta *mapDeltas;
    int        mapDeltaCount;

    /* Pathfinder grids — danger / influence / overlay (65536 entries
     * each). Same keyframe-or-delta pattern. */
    uint16_t  *fullDanger;
    GridDelta *dangerDeltas;
    int        dangerDeltaCount;
    int16_t   *fullInfluence;
    GridDelta *influenceDeltas;
    int        influenceDeltaCount;
    uint16_t  *fullOverlay;
    GridDelta *overlayDeltas;
    int        overlayDeltaCount;

    /* brainMap (fog of war) for the followed bot. */
    BYTE      *fullBrainMap;
    ByteDelta *brainMapDeltas;
    int        brainMapDeltaCount;

    /* Small game state — full copy every frame (cheap). */
    TankSnapshot      tanks[MAX_TANKS];
    uint8_t           tankCount;
    ShellSnapshot     snapShells[MAX_SNAPSHOT_SHELLS];
    uint8_t           shellCount;
    BaseSnapshot      snapBases[MAX_SNAPSHOT_BASES];
    uint8_t           baseCount;
    PillSnapshot      snapPills[MAX_SNAPSHOT_PILLS];
    uint8_t           pillCount;

    /* Camera + brain perf for the HUD. */
    WORLD viewCenterX, viewCenterY;
    float thinkMs;

    /* Goal info for scrubber tick markers. The scrubber detects
     * replan ticks by diffing consecutive frames' goal info, so no
     * explicit wasReplan flag — this branch's BrainGoalInfo doesn't
     * carry one. */
    BrainGoalInfo goalInfo;
    bool          goalInfoValid;

    /* Per-panel JSON the brain returned this tick, indexed by panel
     * registry idx. NULL = panel didn't exist or wasn't polled this
     * frame. Owned by the frame; freed in recordingFreeFrame. */
    char *recordedPanels[PANEL_REG_MAX];

    /* Brain-emitted overlay commands captured per active bot. Without
     * this, scrubbing back would show whatever the brain has emitted
     * MOST RECENTLY (live) — not what it emitted at the scrubbed
     * tick. Stored per-bot so Tab-switching followBot during playback
     * still pulls the right bot's overlays. */
    OverlayCmd *botOverlayCmds[MAX_TANKS];
    int         botOverlayCmdCount[MAX_TANKS];

    /* A* / Dijkstra path snapshot for the green key-4 overlay. */
    int *pathX;
    int *pathY;
    int  pathLen;

    /* Shot-sim POI poll results captured at this tick. The shot-sim
     * panel polls POIs via Lua against the followed bot's live state;
     * playback re-uses these recorded values so the buttons reflect
     * what the bot would have shown at the scrubbed tick instead of
     * the current live state. Indexed by POI registry slot. */
    struct {
        int32_t wx;
        int32_t wy;
        bool    ok;
    } shotSimPois[SHOTSIM_POI_REG_MAX];
    uint8_t shotSimPoiCount;  /* slots populated this frame */

    /* viz_detail registry snapshot (clickable map primitives the
     * brain registered this tick with rich body text). Without
     * recording this, the inspector dialog goes blank during
     * playback because the brain isn't running. malloc'd per frame;
     * freed in recordingFreeFrame. */
    VizDetailEntry *vizDetails;
    int             vizDetailCount;

    /* pill_contrib registry snapshot (per-pill, per-tile danger
     * contribution maps). Same scrub-back rationale as vizDetails:
     * brain isn't running in playback so the shift-2 overlay needs
     * the recorded data to remain functional. malloc'd per frame;
     * freed in recordingFreeFrame. */
    PillContribEntry *pillContrib;
    int               pillContribCount;

    /* Strategic placement heatmap text snapshotted when the [8]
     * overlay is on; NULL otherwise (saves the Lua poll cost). */
    char *stratPlaceText;
} RecordingFrame;

typedef struct {
    RecordingFrame *frames;
    int             count;
    int             capacity;

    /* Previous-frame state for computing deltas. */
    BYTE     *prevMap;
    uint16_t *prevDanger;
    int16_t  *prevInfluence;
    uint16_t *prevOverlay;
    BYTE     *prevBrainMap;
    bool      hasPrev;

    /* Reconstructed playback buffers — populated by
     * recordingReconstructMap before each playback render pass. */
    BYTE     *playbackMap;
    uint16_t *playbackDanger;
    int16_t  *playbackInfluence;
    uint16_t *playbackOverlay;
    BYTE     *playbackBrainMap;
    int       playbackMapFrame;  /* frame the playback buffers reflect */
} RecordingBuffer;

/* ------------------------------------------------------------------ */
/* App state                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    ServerSim    sim;
    bool         simValid;
    SDL_Window  *window;
    SDL_Renderer*renderer;
    SDL_Texture *tilesTex;

    /* Camera (world coords) */
    WORLD        viewCenterX;
    WORLD        viewCenterY;
    bool         freeCamera;     /* true = arrow keys, false = follow bot */
    BYTE         followBot;      /* bot slot to follow */

    /* Map bounds */
    int          mapMinX, mapMinY, mapMaxX, mapMaxY;

    /* Simulation */
    BYTE         numBots;
    bool         paused;
    int          zoomFactor;

    /* Native overlay toggles live in the viz registry alongside
     * brain-registered ones — single source of truth, single INI.
     * We cache each native's registry idx at startup so number-key
     * handlers and renderers can flip / read in O(1) without a
     * string lookup. -1 if registration failed. */
    int          regIdxInfluence;
    int          regIdxDanger;
    int          regIdxFrontLine;
    int          regIdxPath;
    int          regIdxFog;
    int          regIdxValues;
    int          regIdxDijkstra;
    int          regIdxCostTo;
    int          regIdxShellHitbox;
    int          regIdxStratPlace;
    char        *stratPlaceText;    /* cached Lua heatmap string; freed when overlay turns off */
    /* Dijkstra heatmap (7 key) — which slate the UI displays. The
     * brain runs up to DIJKSTRA_NUM_SLATES (4) parallel searches
     * with different parameters; Shift+7 cycles which one is on
     * screen. Wrapped at the slate count. */
    int          dijViewSlate;

    /* cost_to heatmap (5 key). Computed off-thread on a cloned
     * pathfinder so a 16k-budget radial sweep doesn't stall the
     * sim. Grid = -1 (uncomputed), >= BT_COST_INF_THRESHOLD
     * (unreachable / over budget), otherwise the actual cost.
     * Shift+5 lowers danger weight to 0.1 to match the pill-cost
     * estimator. The mutex serializes worker writes against the
     * renderer's per-frame read — a torn float read is technically
     * UB even though aligned 4-byte stores are atomic on x86. */
    float       *costToGrid;
    int          costToTankMX, costToTankMY;
    SDL_Thread  *costToThread;
    SDL_Mutex   *costToMutex;
    bool         costToAbort;
    bool         showHUD;          /* status bar + legend (toggle with H) */
    bool         showShortcuts;    /* keyboard-shortcut help overlay (F1) */
    /* shift-2 cycles through pill_contrib overlays one pill at a time.
     * 0 = off; 1..pillContribCount() selects that pill (1-based for
     * easy keyboard cycling). */
    int          pillContribSel;

    /* Overlay texture (256x256 RGBA, updated once per game tick) */
    SDL_Texture *overlayTex;
    uint32_t     overlayTick;   /* sim tick when overlay was last updated */
    bool         overlayDirty;  /* force update on toggle change */

    /* Cached path (tracePath only works when A* status==1) */
    int          cachedPath_x[2048];
    int          cachedPath_y[2048];
    int          cachedPathLen;

    /* Debug pathfinder (for click-to-cost, independent of brain's PF) */
    BrainPathfinder *debugPF;

    /* Mouse / drag-to-pan state */
    bool         dragging;              /* middle or right drag active */
    bool         rightDown;             /* right button is held */
    bool         rightDragged;          /* right button moved (drag vs click) */
    float        dragLastX, dragLastY;  /* last drag screen position */

    /* Mouse / click-to-cost state */
    float        mouseX, mouseY;        /* screen coords */
    int          hoverMX, hoverMY;      /* map tile under cursor */
    int          hoverWX, hoverWY;      /* full-precision wu (1/256 tile) */
    bool         hoverValid;

    bool         clickActive;           /* a click-cost query is active */
    int          clickMX, clickMY;      /* clicked tile */
    float        clickCost;             /* A* or estimate cost tank → click */
    float        clickEstCost;          /* linear estimate cost (same as brain uses) */
    int          clickPath_x[2048];     /* traced path to clicked tile */
    int          clickPath_y[2048];
    int          clickPathLen;

    /* Goal info (refreshed per tick) */
    BrainGoalInfo goalInfo;
    bool         goalInfoValid;
    float        targetCost;            /* A* cost to current goal target */
    float        targetEstCost;         /* linear estimate (what brain uses for ranking) */

    /* V dialog (visualizations checkbox list) — separate SDL window
     * with its own ImGui context. Created on first 'V' press. */
    SDL_Window  *vizWindow;
    SDL_Renderer*vizRenderer;

    /* P dialog (panel/text dump tabs) — same shape as V, separate
     * window + ImGui context. Tabs come from the panel registry
     * which the brain populates via braintest_panel_register. */
    SDL_Window  *panelWindow;
    SDL_Renderer*panelRenderer;

    /* X-key bare-screen mode: short-circuits all viz overlays except
     * the always-visible HUD-resources overlay. Pressing X again
     * lifts it; the per-id checkbox states are untouched. */
    bool         vizSuppressActive;

    /* Manual control mode (M key). When on, the keyboard talks to
     * the brain via two optional Lua hooks the brain may implement:
     *   brain.set_manual_mode(int)         — flag flipped by M key
     *   brain.manual_key(role_name, bool)  — Forward/Backward/Left/
     *     Right/Shoot/LayMine down/up. Role names are derived from
     *     the user's WinBolo key bindings (loaded from the same INI
     *     the main game uses), so the brain doesn't have to care
     *     what physical key the user has configured.
     * Brains that don't implement them ignore the messages; the
     * flag still flips C-side so callers can query app.manualControl. */
    bool         manualControl;
    /* SDL_Scancode values, loaded from WinBolo.ini [KEYS] at
     * startup. Defaults match the original Mac WinBolo bindings
     * (E forward, D backward, S left, F right). */
    int          keyForward;
    int          keyBackward;
    int          keyLeft;
    int          keyRight;
    int          keyShoot;
    int          keyLayMine;

    /* Ring buffer of the followed bot's last N think-times (ms),
     * captured once per game tick. The CPU-use graph in the top-
     * right HUD plots these as bars so spikes / steady-state
     * trends are visible at a glance. `cpuHistHead` is the index
     * of the next slot to write; the array holds at most
     * CPU_HIST_BARS samples (oldest is overwritten). */
#define CPU_HIST_BARS 200
    float        cpuHist[CPU_HIST_BARS];
    int          cpuHistCount;   /* 0..CPU_HIST_BARS, capped at the buffer size */
    int          cpuHistHead;    /* next-write index, wraps mod CPU_HIST_BARS */

    /* Timeline recording + playback (stage 4).
     *  - recording captures full game state per tick (map, grids,
     *    tanks, shells, bases, pills) plus the brain's per-panel
     *    JSON output. Memory-bounded via delta encoding for the
     *    256x256 grids.
     *  - playbackMode flips the entire renderer over to reading
     *    from a reconstructed snapshot of frame `playbackFrame`
     *    instead of the live sim. Toggling viz in playback works
     *    because the renderers read live viz flags but draw against
     *    the patched-in snapshot data.
     *  - scrubbing is true while the user has the timeline thumb
     *    grabbed; the main loop steers playbackFrame from the mouse
     *    instead of advancing it. */
    RecordingBuffer recording;
    bool         playbackMode;
    int          playbackFrame;
    bool         scrubbing;
    /* Speed control — index into SPEED_PRESETS. Affects both live
     * tick rate and playback frame-step rate. */
    int          speedIndex;

    /* Shot-sim result — owned by main, populated by the panel's
     * Run callback. Drawn over the live map by appRenderOverlay. */
    BrainShotTile shotTiles[8192];
    int           shotTileCount;
    bool          shotHasResult;
    int           shotOriginWX, shotOriginWY;
    int           shotTargetWX, shotTargetWY;
    int           shotShooterType;
    /* (goalInfo / goalInfoValid live higher up in the struct already
     * — they're refreshed per tick before recordingCapture and the
     * scrubber's goal-change tick markers read from the captured
     * frames, not the live cache.) */
} BrainTestApp;

/* Common "is this cost infinite?" cutoff for the heatmap renderers.
 * Real path costs top out around the low thousands; brain_pathfinder
 * uses COST_INF = 1e30f, and brainPathfinderCostTo treats anything
 * past `budget` as effectively unreachable. Anything above 1e20 is
 * unambiguously a sentinel, regardless of which API produced it. */
#define BT_COST_INF_THRESHOLD 1e20f

/* Registry-backed accessor + flip for the native toggles. Returns
 * false if the row hasn't been registered (shouldn't happen — we
 * register them all at startup; a -1 here usually means the registry
 * is full or duplicate-id collision logged at startup). */
static bool vizFlag(int idx) {
    const VizRegistryEntry *e = vizRegistryGet(idx);
    return e ? e->is_on : false;
}
static void vizFlagFlip(int idx) {
    VizRegistryEntry *e = vizRegistryGetMutable(idx);
    if (e) e->is_on = !e->is_on;
}

static volatile bool appQuit = FALSE;

/* ────────────────────────────────────────────────────────────── */
/* Recording (per-tick capture) + playback reconstruction.        */
/* All ported from optimize-perf branch. See struct comment above */
/* for the design overview.                                        */
/* ────────────────────────────────────────────────────────────── */

/* Forward decl — defined alongside the shot-sim panel callbacks below.
 * Needed here because recordingCapture polls every POI per tick. */
static bool shotSimPoiPollLive(int idx, BrainTestApp *app, int *outWX, int *outWY);

static void recordingInit(RecordingBuffer *rb) {
    memset(rb, 0, sizeof(*rb));
    rb->playbackMapFrame = -1;
    int mapSz  = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
    int gridSz = 65536;
    rb->prevMap          = (BYTE *)calloc(mapSz, 1);
    rb->prevDanger       = (uint16_t *)calloc(gridSz, sizeof(uint16_t));
    rb->prevInfluence    = (int16_t  *)calloc(gridSz, sizeof(int16_t));
    rb->prevOverlay      = (uint16_t *)calloc(gridSz, sizeof(uint16_t));
    rb->prevBrainMap     = (BYTE *)calloc(mapSz, 1);
    rb->playbackMap      = (BYTE *)calloc(mapSz, 1);
    rb->playbackDanger   = (uint16_t *)calloc(gridSz, sizeof(uint16_t));
    rb->playbackInfluence= (int16_t  *)calloc(gridSz, sizeof(int16_t));
    rb->playbackOverlay  = (uint16_t *)calloc(gridSz, sizeof(uint16_t));
    rb->playbackBrainMap = (BYTE *)calloc(mapSz, 1);
}

static void recordingFreeFrame(RecordingFrame *f) {
    free(f->fullMap);
    free(f->mapDeltas);
    free(f->fullDanger);
    free(f->dangerDeltas);
    free(f->fullInfluence);
    free(f->influenceDeltas);
    free(f->fullOverlay);
    free(f->overlayDeltas);
    free(f->fullBrainMap);
    free(f->brainMapDeltas);
    for (int i = 0; i < PANEL_REG_MAX; i++) free(f->recordedPanels[i]);
    for (int i = 0; i < MAX_TANKS; i++) free(f->botOverlayCmds[i]);
    free(f->pathX);
    free(f->pathY);
    free(f->vizDetails);
    free(f->pillContrib);
    SDL_free(f->stratPlaceText);
    memset(f, 0, sizeof(*f));
}

static void recordingDestroy(RecordingBuffer *rb) {
    for (int i = 0; i < rb->count; i++) recordingFreeFrame(&rb->frames[i]);
    free(rb->frames);
    free(rb->prevMap);
    free(rb->prevDanger);
    free(rb->prevInfluence);
    free(rb->prevOverlay);
    free(rb->prevBrainMap);
    free(rb->playbackMap);
    free(rb->playbackDanger);
    free(rb->playbackInfluence);
    free(rb->playbackOverlay);
    free(rb->playbackBrainMap);
    memset(rb, 0, sizeof(*rb));
    rb->playbackMapFrame = -1;
}

/* recordingCapture lives further down (near pushVizStateToBots) —
 * it touches BrainTestApp fields that aren't visible up here yet. */

/* Reconstruct rb->playbackMap / playbackDanger / playbackInfluence
 * / playbackOverlay / playbackBrainMap to the state at frame
 * `targetFrame`. Walks back to the nearest keyframe at or before
 * targetFrame, then applies deltas forward. Idempotent — bails
 * early if the buffers already reflect targetFrame. */
static void recordingReconstructMap(RecordingBuffer *rb, int targetFrame) {
    if (targetFrame < 0 || targetFrame >= rb->count) return;
    if (rb->playbackMapFrame == targetFrame) return;

    int kf = targetFrame;
    while (kf >= 0 && !rb->frames[kf].isKeyframe) kf--;
    if (kf < 0) return;

    int mapSz  = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
    int gridSz = 65536;
    if (rb->frames[kf].fullMap)
        memcpy(rb->playbackMap, rb->frames[kf].fullMap, mapSz);
    if (rb->frames[kf].fullDanger)
        memcpy(rb->playbackDanger, rb->frames[kf].fullDanger,
               gridSz * sizeof(uint16_t));
    else
        memset(rb->playbackDanger, 0, gridSz * sizeof(uint16_t));
    if (rb->frames[kf].fullInfluence)
        memcpy(rb->playbackInfluence, rb->frames[kf].fullInfluence,
               gridSz * sizeof(int16_t));
    else
        memset(rb->playbackInfluence, 0, gridSz * sizeof(int16_t));
    if (rb->frames[kf].fullOverlay)
        memcpy(rb->playbackOverlay, rb->frames[kf].fullOverlay,
               gridSz * sizeof(uint16_t));
    else
        memset(rb->playbackOverlay, 0, gridSz * sizeof(uint16_t));
    if (rb->frames[kf].fullBrainMap)
        memcpy(rb->playbackBrainMap, rb->frames[kf].fullBrainMap, mapSz);
    else
        memset(rb->playbackBrainMap, 0xFF, mapSz);

    for (int i = kf + 1; i <= targetFrame; i++) {
        RecordingFrame *df = &rb->frames[i];
        for (int d = 0; d < df->mapDeltaCount; d++)
            rb->playbackMap[df->mapDeltas[d].offset] = df->mapDeltas[d].newVal;
        for (int d = 0; d < df->dangerDeltaCount; d++)
            rb->playbackDanger[df->dangerDeltas[d].offset] =
                (uint16_t)df->dangerDeltas[d].newVal;
        for (int d = 0; d < df->influenceDeltaCount; d++)
            rb->playbackInfluence[df->influenceDeltas[d].offset] =
                df->influenceDeltas[d].newVal;
        for (int d = 0; d < df->overlayDeltaCount; d++)
            rb->playbackOverlay[df->overlayDeltas[d].offset] =
                (uint16_t)df->overlayDeltas[d].newVal;
        for (int d = 0; d < df->brainMapDeltaCount; d++)
            rb->playbackBrainMap[df->brainMapDeltas[d].offset] =
                df->brainMapDeltas[d].newVal;
    }
    rb->playbackMapFrame = targetFrame;
}

/* Drop frames at-and-after `keepCount`. Used when the user scrubs
 * back and resumes — the future is now stale.
 *
 * After truncating, the prev* buffers (which the next capture
 * computes deltas against) need to reflect the LAST KEPT frame's
 * state — not the dropped tail's. recordingReconstructMap fills
 * playback* with that exact state, so we can copy each playback*
 * into its corresponding prev* in one shot. Without this the next
 * frame's deltas would be computed against ghost data and replay
 * would show stale grid cells. */
static void recordingTruncate(RecordingBuffer *rb, int keepCount) {
    for (int i = keepCount; i < rb->count; i++) recordingFreeFrame(&rb->frames[i]);
    rb->count = keepCount;
    rb->playbackMapFrame = -1;
    if (keepCount > 0) {
        int mapSz  = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
        int gridSz = 65536;
        recordingReconstructMap(rb, keepCount - 1);
        memcpy(rb->prevMap,       rb->playbackMap,       mapSz);
        memcpy(rb->prevDanger,    rb->playbackDanger,    gridSz * sizeof(uint16_t));
        memcpy(rb->prevInfluence, rb->playbackInfluence, gridSz * sizeof(int16_t));
        memcpy(rb->prevOverlay,   rb->playbackOverlay,   gridSz * sizeof(uint16_t));
        memcpy(rb->prevBrainMap,  rb->playbackBrainMap,  mapSz);
        rb->hasPrev = true;
    } else {
        rb->hasPrev = false;
    }
}

/* Read the user's WinBolo key bindings from the same INI the main
 * game uses, so manual control in BrainTest matches the muscle
 * memory the user already has. Falls back to defaults if the file
 * or the [KEYS] section is missing. Stored as SDL_Scancode values. */
static void loadKeyBindings(BrainTestApp *app) {
    /* Defaults match Bolo's Mac defaults — E forward, D backward,
     * S left, F right, Space shoot, LShift mine. */
    app->keyForward  = SDL_SCANCODE_E;
    app->keyBackward = SDL_SCANCODE_D;
    app->keyLeft     = SDL_SCANCODE_S;
    app->keyRight    = SDL_SCANCODE_F;
    app->keyShoot    = SDL_SCANCODE_SPACE;
    app->keyLayMine  = SDL_SCANCODE_LSHIFT;

#ifdef _WIN32
    char buff[64];
    char iniPath[FILENAME_MAX];
    /* SDL_GetPrefPath returns a heap-allocated string; must free. */
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir && prefDir[0]) {
        SDL_snprintf(iniPath, sizeof(iniPath), "%sWinBolo.ini", prefDir);
    } else {
        SDL_snprintf(iniPath, sizeof(iniPath), "WinBolo.ini");
    }
    SDL_free(prefDir);
    /* The values stored in the INI by the main game are SDL
     * scancodes (the same numbers SDL3 uses). Default fallbacks
     * are scancodes for E/D/S/F/Space/LShift respectively. */
    GetPrivateProfileStringA("KEYS", "Forward",   "8",   buff, sizeof(buff), iniPath); /* E */
    app->keyForward  = atoi(buff);
    GetPrivateProfileStringA("KEYS", "Backwards", "7",   buff, sizeof(buff), iniPath); /* D */
    app->keyBackward = atoi(buff);
    GetPrivateProfileStringA("KEYS", "Left",      "22",  buff, sizeof(buff), iniPath); /* S */
    app->keyLeft     = atoi(buff);
    GetPrivateProfileStringA("KEYS", "Right",     "9",   buff, sizeof(buff), iniPath); /* F */
    app->keyRight    = atoi(buff);
    GetPrivateProfileStringA("KEYS", "Shoot",     "44",  buff, sizeof(buff), iniPath); /* Space */
    app->keyShoot    = atoi(buff);
    GetPrivateProfileStringA("KEYS", "Lay Mine",  "225", buff, sizeof(buff), iniPath); /* LShift */
    app->keyLayMine  = atoi(buff);
#endif

    SDL_Log("Manual key bindings (scancodes): fwd=%d back=%d left=%d "
            "right=%d shoot=%d mine=%d",
            app->keyForward, app->keyBackward, app->keyLeft, app->keyRight,
            app->keyShoot, app->keyLayMine);
}

/* SDL_Scancode → role name lookup. Returns NULL if the scancode
 * isn't bound to any of the manual control roles. */
static const char *keyToRole(BrainTestApp *app, int scancode) {
    if (scancode == app->keyForward)  return "forward";
    if (scancode == app->keyBackward) return "backward";
    if (scancode == app->keyLeft)     return "left";
    if (scancode == app->keyRight)    return "right";
    if (scancode == app->keyShoot)    return "shoot";
    if (scancode == app->keyLayMine)  return "lay_mine";
    return NULL;
}

/* Forward decl — defined later with renderBrainOverlay. */
static void mapTileToScreen(BrainTestApp *app, float tx, float ty,
                            int screenW, int screenH,
                            float *out_sx, float *out_sy);

/* Cell-value numeric overlay (toggled with 9). Prints the raw
 * danger / influence value on every visible tile when the
 * corresponding grid overlay is on. Skipped at low zoom (numbers
 * unreadable). Generic — reads only BrainPathfinder grids. */
static void renderCellValues(BrainTestApp *app, int screenW, int screenH) {
    if (!vizFlag(app->regIdxValues)) return;
    if (!vizFlag(app->regIdxDanger) && !vizFlag(app->regIdxInfluence)) return;
    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (!pf) return;

    int zf = app->zoomFactor;
    int tilePx = 16 * zf;
    if (tilePx < 16) return;     /* too small to read */

    int centerMX = app->viewCenterX >> 8;
    int centerMY = app->viewCenterY >> 8;
    int tilesW = screenW / tilePx + 2;
    int tilesH = screenH / tilePx + 2;
    int startMX = centerMX - tilesW / 2;
    int startMY = centerMY - tilesH / 2;

    float textScale = (tilePx >= 32) ? 1.0f : 0.5f;
    SDL_SetRenderScale(app->renderer, textScale, textScale);

    char buf[16];
    for (int ty = startMY; ty < startMY + tilesH; ty++) {
        if (ty < 0 || ty > 255) continue;
        for (int tx = startMX; tx < startMX + tilesW; tx++) {
            if (tx < 0 || tx > 255) continue;
            int idx = ty * 256 + tx;
            bool drawn = false;
            if (vizFlag(app->regIdxDanger)) {
                uint16_t d = pf->danger_grid[idx];
                if (d > 0) {
                    float sx, sy;
                    mapTileToScreen(app, (float)tx + 0.1f,
                                          (float)ty + 0.2f,
                                          screenW, screenH, &sx, &sy);
                    SDL_snprintf(buf, sizeof(buf), "%d", d);
                    SDL_SetRenderDrawColor(app->renderer, 255, 140, 0, 255);
                    SDL_RenderDebugText(app->renderer,
                        sx / textScale, sy / textScale, buf);
                    drawn = true;
                }
            }
            if (vizFlag(app->regIdxInfluence)) {
                int16_t inf = pf->influence_grid[idx];
                if (inf != 0) {
                    float sx, sy;
                    float yOff = drawn ? 0.6f : 0.2f;
                    mapTileToScreen(app, (float)tx + 0.1f,
                                          (float)ty + yOff,
                                          screenW, screenH, &sx, &sy);
                    SDL_snprintf(buf, sizeof(buf), "%d", inf);
                    if (inf > 0) SDL_SetRenderDrawColor(app->renderer,  80, 140, 255, 255);
                    else         SDL_SetRenderDrawColor(app->renderer, 255,  80,  80, 255);
                    SDL_RenderDebugText(app->renderer,
                        sx / textScale, sy / textScale, buf);
                }
            }
        }
    }
    SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
}

/* Common per-frame projection used by every full-map overlay: tile
 * pixel size, screen origin of map tile (0,0), and the visible tile
 * range clamped to [0..255]. Three renderers reproduced this math
 * verbatim before — keep them in sync via this one helper. */
static void mapTileWindow(BrainTestApp *app, int screenW, int screenH,
                          int *outTilePx, float *outMoX, float *outMoY,
                          int *outStartX, int *outStartY,
                          int *outEndX, int *outEndY) {
    int   tp  = 16 * app->zoomFactor;
    float scx = screenW / 2.0f;
    float scy = screenH / 2.0f;
    float moX = scx - (app->viewCenterX >> 8) * tp
        - (float)(app->viewCenterX & 0xFF) * tp / 256.0f;
    float moY = scy - (app->viewCenterY >> 8) * tp
        - (float)(app->viewCenterY & 0xFF) * tp / 256.0f;
    int sx = (int)((-moX) / tp) - 1;
    int sy = (int)((-moY) / tp) - 1;
    int ex = sx + screenW / tp + 3;
    int ey = sy + screenH / tp + 3;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (ex > 255) ex = 255;
    if (ey > 255) ey = 255;
    *outTilePx = tp; *outMoX = moX; *outMoY = moY;
    *outStartX = sx; *outStartY = sy; *outEndX = ex; *outEndY = ey;
}

/* Fog of war overlay: tile-by-tile dark fill on every cell that's
 * still 0xFF (unseen) in the followed bot's pathfinder map.
 * Generic — works for any brain that maintains a worldPtr to a
 * 256×256 byte map (which the pathfinder API guarantees: see
 * brainPathfinderSetMap). */
static void renderFogOverlay(BrainTestApp *app, int screenW, int screenH) {
    if (!vizFlag(app->regIdxFog)) return;
    BrainPathfinder *bpf = botManagerGetBrainPathfinder(app->followBot);
    if (!bpf || !bpf->map) return;

    int   tp, startX, startY, endX, endY;
    float moX, moY;
    mapTileWindow(app, screenW, screenH, &tp, &moX, &moY,
                  &startX, &startY, &endX, &endY);

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 180);
    for (int y = startY; y <= endY; y++) {
        for (int x = startX; x <= endX; x++) {
            BYTE raw = bpf->map[y * 256 + x];
            /* "Unseen" = literal 0xFF sentinel OR low-nibble == 15
             * (the unknown terrain marker the pathfinder uses). */
            if (raw == 0xFF || (raw & 0x0F) == 15) {
                SDL_FRect rect = { moX + x * tp, moY + y * tp,
                                   (float)tp, (float)tp };
                SDL_RenderFillRect(app->renderer, &rect);
            }
        }
    }
}

static void syncDebugPathfinder(BrainTestApp *app);

/* ── cost_to heatmap background thread (key 5) ──
 * Runs brainPathfinderCostTo from the bot's tile out to neighbors
 * in expanding-ring order so the heatmap fills outward visibly.
 * Operates on a *cloned* pathfinder with its own copy of the bot's
 * map + cost tables — the live PF would otherwise be mutated by an
 * A* search running concurrently on the main thread (cost_to and
 * pathTo share the search arrays). Stops when ten consecutive
 * rings produced nothing finite (search front exhausted) or when
 * the abort flag flips. */
typedef struct {
    BrainPathfinder *pf;
    BYTE      *mapCopy;
    float     *grid;
    SDL_Mutex *gridMutex;          /* shared with main thread */
    int        tmx, tmy, in_boat;
    int        shells, trees, mines, armour;
    int        budget;
    bool      *abort;
} CostToThreadCtx;

static int SDLCALL costToThreadFunc(void *data) {
    CostToThreadCtx *ctx = (CostToThreadCtx *)data;
    int consInf = 0;
    /* Buffer one ring's worth of results, then publish under the
     * mutex in a single batch. Keeps the renderer's lock window
     * tiny and avoids lock-per-cell churn. Worst case is the
     * outermost ring (4*r cells), which at r=127 is ~508 entries. */
    int   bufX[512 * 4], bufY[512 * 4];
    float bufC[512 * 4];
    for (int r = 1; r < 128 && consInf < 10; r++) {
        if (*ctx->abort) break;
        bool anyFinite = false;
        int  bufN = 0;
        for (int dx = -r; dx <= r; dx++) {
            for (int dy = -r; dy <= r; dy++) {
                if (abs(dx) != r && abs(dy) != r) continue;
                if (*ctx->abort) goto done;
                int x = ctx->tmx + dx, y = ctx->tmy + dy;
                if (x < 0 || x > 255 || y < 0 || y > 255) continue;
                float c = brainPathfinderCostTo(ctx->pf,
                    ctx->tmx, ctx->tmy, x, y, ctx->in_boat,
                    ctx->shells, ctx->trees, ctx->mines, ctx->armour,
                    ctx->budget);
                if (bufN < (int)(sizeof(bufX) / sizeof(bufX[0]))) {
                    bufX[bufN] = x; bufY[bufN] = y; bufC[bufN] = c;
                    bufN++;
                }
                if (c < BT_COST_INF_THRESHOLD) anyFinite = true;
            }
        }
        if (ctx->gridMutex) SDL_LockMutex(ctx->gridMutex);
        for (int i = 0; i < bufN; i++) {
            ctx->grid[bufY[i] * 256 + bufX[i]] = bufC[i];
        }
        if (ctx->gridMutex) SDL_UnlockMutex(ctx->gridMutex);

        if (!anyFinite) consInf++;
        else            consInf = 0;
        SDL_Delay(1); /* let the renderer breathe between rings */
    }
done:
    brainPathfinderDestroy(ctx->pf);
    free(ctx->mapCopy);
    free(ctx);
    return 0;
}

/* Renders whatever cells the thread has filled in so far. Uses the
 * same green→red gradient as the Dijkstra heatmap; INF cells (over
 * budget / unreachable) render as solid red so the user can see the
 * boundary of what's actually reachable. */
static void renderCostToHeatmap(BrainTestApp *app, int screenW, int screenH) {
    if (!vizFlag(app->regIdxCostTo) || !app->costToGrid) return;

    int   tp, startX, startY, endX, endY;
    float moX, moY;
    mapTileWindow(app, screenW, screenH, &tp, &moX, &moY,
                  &startX, &startY, &endX, &endY);
    int zf = app->zoomFactor;

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    /* Snapshot the visible window under the mutex so the worker
     * can't mid-write a float we're about to read. Worst case is
     * the full 256×256 grid (very low zoom on a huge window), so
     * the snapshot buffer is sized to match. */
    int  spanX = endX - startX + 1;
    int  spanY = endY - startY + 1;
    if (spanX <= 0 || spanY <= 0) return;
    static float snap[256 * 256];
    if (app->costToMutex) SDL_LockMutex(app->costToMutex);
    for (int y = 0; y < spanY; y++) {
        memcpy(&snap[y * spanX],
               &app->costToGrid[(startY + y) * 256 + startX],
               spanX * sizeof(float));
    }
    if (app->costToMutex) SDL_UnlockMutex(app->costToMutex);

    for (int y = 0; y < spanY; y++) {
        for (int x = 0; x < spanX; x++) {
            float c = snap[y * spanX + x];
            if (c < 0) continue; /* not yet computed */

            int r, g, b, a;
            if (c >= BT_COST_INF_THRESHOLD) {
                r = 255; g = 0; b = 0; a = 120;
            } else {
                float t = c / 500.0f;
                if (t > 1.0f) t = 1.0f;
                r = (int)(t * 255);
                g = (int)((1.0f - t) * 255);
                b = 0; a = 80;
            }
            SDL_SetRenderDrawColor(app->renderer, r, g, b, a);
            SDL_FRect rect = { moX + (startX + x) * tp,
                               moY + (startY + y) * tp,
                               (float)tp, (float)tp };
            SDL_RenderFillRect(app->renderer, &rect);

            if (zf >= 2) {
                char buf[16];
                if (c >= BT_COST_INF_THRESHOLD)
                    SDL_snprintf(buf, sizeof(buf), "INF");
                else
                    SDL_snprintf(buf, sizeof(buf), "%.0f", c);
                int tw = (int)strlen(buf) * 8;
                int th = 8;
                float tx = rect.x + (tp - tw) * 0.5f;
                float ty = rect.y + (tp - th) * 0.5f;
                SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 200);
                SDL_RenderDebugText(app->renderer, tx, ty, buf);
            }
        }
    }
}

/* Kick off a cost_to computation. Stops any in-flight thread first
 * so a re-press always restarts from the current tank position. */
static void startCostToHeatmap(BrainTestApp *app, bool lowDanger) {
    if (app->costToThread) {
        app->costToAbort = true;
        SDL_WaitThread(app->costToThread, NULL);
        app->costToThread = NULL;
    }

    syncDebugPathfinder(app);
    BrainPathfinder *src = app->debugPF;
    if (!src || !src->map) return;

    /* Origin selection: in playback, anchor to the followed tank's
     * RECORDED position at the scrubbed frame (not the live sim's
     * frozen position). Live mode keeps the previous behavior. */
    WORLD twx = 0, twy = 0;
    int in_boat = 0;
    bool haveOrigin = false;
    if (app->playbackMode &&
        app->playbackFrame >= 0 &&
        app->playbackFrame < app->recording.count) {
        const RecordingFrame *pf_ = &app->recording.frames[app->playbackFrame];
        for (int i = 0; i < pf_->tankCount; i++) {
            if (pf_->tanks[i].playerNum == app->followBot) {
                twx = pf_->tanks[i].worldX;
                twy = pf_->tanks[i].worldY;
                in_boat = (pf_->tanks[i].tankStatus & 0x0F) ? 1 : 0;
                haveOrigin = true;
                break;
            }
        }
    }
    if (!haveOrigin) {
        if (!serverSimGetTankState(&app->sim, app->followBot, &twx, &twy)) return;
        in_boat = (app->sim.sim.tanks[app->followBot] != NULL &&
                   tankIsOnBoat(&app->sim.sim.tanks[app->followBot])) ? 1 : 0;
    }
    int smx = twx >> 8;
    int smy = twy >> 8;

    if (!app->costToGrid) {
        app->costToGrid = (float *)malloc(256 * 256 * sizeof(float));
        if (!app->costToGrid) return;
    }
    if (!app->costToMutex) app->costToMutex = SDL_CreateMutex();
    /* Reset under the lock — even though no worker can be running
     * right now (we waited above), a paranoid future change might
     * spawn the worker before this loop finishes. Cheap insurance. */
    if (app->costToMutex) SDL_LockMutex(app->costToMutex);
    for (int i = 0; i < 256 * 256; i++) app->costToGrid[i] = -1.0f;
    if (app->costToMutex) SDL_UnlockMutex(app->costToMutex);
    app->costToTankMX = smx;
    app->costToTankMY = smy;

    BrainPathfinder *clone = brainPathfinderCreate();
    if (!clone) return;
    BYTE *mapCopy = (BYTE *)malloc(256 * 256);
    if (!mapCopy) { brainPathfinderDestroy(clone); return; }
    memcpy(mapCopy, src->map, 256 * 256);
    brainPathfinderSetMap(clone, mapCopy);
    memcpy(clone->terrain_cost_table,      src->terrain_cost_table,      sizeof(src->terrain_cost_table));
    memcpy(clone->terrain_cost_boat_table, src->terrain_cost_boat_table, sizeof(src->terrain_cost_boat_table));
    memcpy(clone->terrain_speed_table,     src->terrain_speed_table,     sizeof(src->terrain_speed_table));
    memcpy(clone->danger_grid,             src->danger_grid,             sizeof(src->danger_grid));
    memcpy(clone->influence_grid,          src->influence_grid,          sizeof(src->influence_grid));
    clone->turn_cost          = src->turn_cost;
    clone->wall_shoot_cost    = src->wall_shoot_cost;
    clone->wall_shoot_shells  = src->wall_shoot_shells;
    clone->mine_penalty       = src->mine_penalty;
    clone->shell_reserve      = src->shell_reserve;
    clone->tree_reserve       = src->tree_reserve;
    clone->water_drain_rate   = src->water_drain_rate;
    clone->shell_loss_cost    = src->shell_loss_cost;
    clone->mine_loss_cost     = src->mine_loss_cost;
    clone->armour_drain_rate  = src->armour_drain_rate;
    clone->road_build_cost    = src->road_build_cost;
    clone->min_shells         = src->min_shells;
    clone->min_mines          = src->min_mines;
    clone->min_armour         = src->min_armour;
    clone->danger_scale       = lowDanger ? 0.1f : src->danger_scale;

    CostToThreadCtx *ctx = (CostToThreadCtx *)calloc(1, sizeof(CostToThreadCtx));
    if (!ctx) { brainPathfinderDestroy(clone); free(mapCopy); return; }
    ctx->pf        = clone;
    ctx->mapCopy   = mapCopy;
    ctx->grid      = app->costToGrid;
    ctx->gridMutex = app->costToMutex;
    ctx->tmx       = smx;
    ctx->tmy      = smy;
    ctx->in_boat  = in_boat;
    /* Same defaults as computeClickPath — the live BrainInfo accessor
     * isn't exposed in this branch and these match a typical mid-game
     * resource load close enough for visualization purposes. */
    ctx->shells   = 40;
    ctx->trees    = 20;
    ctx->mines    = 20;
    ctx->armour   = 40;
    ctx->budget   = 16000;
    ctx->abort    = &app->costToAbort;
    app->costToAbort = false;
    app->costToThread = SDL_CreateThread(costToThreadFunc, "CostTo", ctx);
}

/* Live Dijkstra heatmap (key 7): per-tile cost-to-every-tile from
 * the followed bot's incremental Dijkstra. Pure O(1) array lookups
 * per tile via brainPathfinderDijkstraCostAt — no thread, no copy.
 * We sample both land and boat layers and display whichever is
 * cheaper. Shift+7 cycles which slate (0..3) is being viewed. */
static void renderDijkstraHeatmap(BrainTestApp *app, int screenW, int screenH) {
    if (!vizFlag(app->regIdxDijkstra)) return;
    BrainPathfinder *botPf = botManagerGetBrainPathfinder(app->followBot);
    if (!botPf) return;

    int viewSlate = app->dijViewSlate;
    int expanded = 0, peakOpen = 0, done = 0;
    int active = brainPathfinderDijkstraStatus(botPf, viewSlate,
                                               &expanded, &peakOpen, &done);

    /* Status label — shows which slate is being viewed, its kind, age,
     * and progress. Drawn even when the slate is INACTIVE so the user
     * can see "I cycled to slate 2 but it has nothing" rather than
     * silent absence. Cyan + top-left, just under the bot/zoom status
     * line at the very top. Format mirrors the original optimize-perf
     * branch verbatim so muscle memory carries over. */
    {
        int dijKind = botPf->dij_slates[viewSlate].kind;
        uint32_t dijStarted = botPf->dij_slates[viewSlate].started_tick;
        uint32_t curTick    = app->sim.tick / 2;
        int life = (dijStarted > 0) ? (int)(curTick - dijStarted) : 0;
        char dijLabel[160];
        SDL_snprintf(dijLabel, sizeof(dijLabel),
            "[7] dijkstra slate=%d kind=%d %s life=%d exp=%d peak_open=%d %s",
            viewSlate, dijKind,
            active ? "ACTIVE" : "INACTIVE",
            life, expanded, peakOpen,
            done ? "DONE" : "(running)");
        SDL_SetRenderDrawColor(app->renderer, 0, 255, 255, 255);
        SDL_RenderDebugText(app->renderer, 4.0f, 144.0f, dijLabel);
    }
    if (!active) return;

    int   tp, startX, startY, endX, endY;
    float moX, moY;
    mapTileWindow(app, screenW, screenH, &tp, &moX, &moY,
                  &startX, &startY, &endX, &endY);
    int zf = app->zoomFactor;

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    for (int y = startY; y <= endY; y++) {
        for (int x = startX; x <= endX; x++) {
            float landC = brainPathfinderDijkstraCostAt(botPf, viewSlate, x, y, 0);
            float boatC = brainPathfinderDijkstraCostAt(botPf, viewSlate, x, y, 1);
            float c = (boatC < landC) ? boatC : landC;
            if (c >= BT_COST_INF_THRESHOLD) continue; /* not yet reached */

            float t = c / 500.0f;
            if (t > 1.0f) t = 1.0f;
            int r = (int)(t * 255);
            int g = (int)((1.0f - t) * 255);
            SDL_SetRenderDrawColor(app->renderer, r, g, 60, 90);
            SDL_FRect rect = { moX + x * tp, moY + y * tp,
                               (float)tp, (float)tp };
            SDL_RenderFillRect(app->renderer, &rect);

            if (zf >= 2) {
                char numBuf[16];
                SDL_snprintf(numBuf, sizeof(numBuf), "%.0f", c);
                int tw = (int)strlen(numBuf) * 8;
                int th = 8;
                float tx = rect.x + (tp - tw) * 0.5f;
                float ty = rect.y + (tp - th) * 0.5f;
                SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 200);
                SDL_RenderDebugText(app->renderer, tx, ty, numBuf);
            }
        }
    }
}

/* Strategic placement heatmap (key 8): scores from Lua's
 * brain.get_strategic_place_heatmap(), rendered as a green→red tile
 * overlay. Best tile highlighted in yellow. */
static void renderStratPlaceHeatmap(BrainTestApp *app, int screenW, int screenH) {
    if (!vizFlag(app->regIdxStratPlace)) return;

    const char *stratText = NULL;
    if (app->playbackMode && app->playbackFrame < app->recording.count) {
        stratText = app->recording.frames[app->playbackFrame].stratPlaceText;
    } else {
        static Uint64 sLastStratPoll = 0;
        Uint64 nowMs = SDL_GetTicks();
        if (!app->stratPlaceText || nowMs - sLastStratPoll > 500) {
            sLastStratPoll = nowMs;
            SDL_free(app->stratPlaceText);
            app->stratPlaceText = botManagerEvalLuaString(app->followBot,
                "return brain.get_strategic_place_heatmap()");
        }
        stratText = app->stratPlaceText;
    }
    if (!stratText) return;

    int zf = app->zoomFactor;
    int tp = 16 * zf;
    int gameH = screenH - CONTROL_BAR_HEIGHT;
    float scx = screenW / 2.0f;
    float scy = gameH / 2.0f;
    float moX = scx - (app->viewCenterX >> 8) * tp
        - (float)(app->viewCenterX & 0xFF) * tp / 256.0f;
    float moY = scy - (app->viewCenterY >> 8) * tp
        - (float)(app->viewCenterY & 0xFF) * tp / 256.0f;

    float minScore = 1e30f, maxScore = -1e30f;
    int bestMX = -1, bestMY = -1;
    float bestScore = -1e30f;
    const char *scan = stratText;
    while (scan && *scan) {
        int mx, my; float sc;
        if (sscanf(scan, "%d\t%d\t%f", &mx, &my, &sc) == 3 && mx >= 0) {
            if (sc < minScore) minScore = sc;
            if (sc > maxScore) maxScore = sc;
            if (sc > bestScore) { bestScore = sc; bestMX = mx; bestMY = my; }
        }
        scan = strchr(scan, '\n');
        if (scan) scan++;
    }
    float range = maxScore - minScore;
    if (range < 1.0f) range = 1.0f;

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    scan = stratText;
    while (scan && *scan) {
        int mx, my; float sc;
        if (sscanf(scan, "%d\t%d\t%f", &mx, &my, &sc) == 3 && mx >= 0) {
            float t = (sc - minScore) / range;
            int r = (int)((1.0f - t) * 255);
            int g = (int)(t * 255);
            int b = 0;
            int a = 100;
            if (mx == bestMX && my == bestMY) { r = 255; g = 255; b = 0; a = 200; }
            float px = moX + mx * tp;
            float py = moY + my * tp;
            SDL_SetRenderDrawColor(app->renderer, r, g, b, a);
            SDL_FRect rect = { px, py, (float)tp, (float)tp };
            SDL_RenderFillRect(app->renderer, &rect);
            if (zf >= 2) {
                char numBuf[16];
                SDL_snprintf(numBuf, sizeof(numBuf), "%.0f", sc);
                int tw = (int)strlen(numBuf) * 8;
                SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 200);
                SDL_RenderDebugText(app->renderer,
                    px + (tp - tw) * 0.5f, py + (tp - 8) * 0.5f, numBuf);
            }
        }
        scan = strchr(scan, '\n');
        if (scan) scan++;
    }
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 0, 255);
    SDL_RenderDebugText(app->renderer, 4, gameH - 20, "[8] Strategic Pill Placement");
}

/* ---------------- Viz registry callback + Lua state push ----------
 * The brain calls braintest_viz_register("id", ...) at brain.open().
 * Our callback adds the row to the registry. We also need to push
 * each row's current state back to the brain as _BT_VIZ_<UPPER_ID>
 * so viz.lua's is_on() check sees it. The brain stamps the registry
 * idx onto every overlay command (via the trailing viz_idx arg the
 * Lua wrappers thread through), and our renderer filters by that
 * idx using vizRegistryGet(idx)->is_on. */

static void vizConfigPath(char *out, size_t n) {
    /* SDL_GetPrefPath returns a heap-allocated string; must free. */
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir && prefDir[0]) {
        SDL_snprintf(out, n, "%sBrainTestViz.ini", prefDir);
    } else {
        SDL_snprintf(out, n, "BrainTestViz.ini");
    }
    SDL_free(prefDir);
}

static void vizToggleSaveCallback(int idx) {
    (void)idx;
    char path[FILENAME_MAX];
    vizConfigPath(path, sizeof(path));
    vizRegistrySaveIni(path);
}

/* Push every entry's on/off state + the id→idx lookup table + the
 * X-key suppress flag into every active bot's Lua state. Also
 * pushes _BT_VIZ_IDS (the table viz.lua's vid() reads to learn
 * which integer to stamp on each overlay command). */
static void pushVizStateToBots(bool vizSuppressActive);

static int vizRegisterCallback(const char *id, const char *label,
                                const char *short_desc, const char *long_desc,
                                int default_on) {
    int existed_before = (vizRegistryFind(id) >= 0);
    int idx = vizRegistryAddBrain(id, label, short_desc, long_desc,
                                   default_on ? true : false);
    if (idx >= 0 && !existed_before) {
        /* Apply persisted state from INI (load merges over defaults). */
        char path[FILENAME_MAX];
        vizConfigPath(path, sizeof(path));
        vizRegistryLoadIni(path);
    }
    return idx;
}

/* While bots are being created the host sets these so the panel
 * registration callback can tag the entry with the right owner +
 * namespace the type with the brain's name. Both reset to -1 / ""
 * after each bot's brain.open() returns so a stray Lua call
 * outside that window can't accidentally claim ownership.
 * Also set around each bot's brain.think via the preThinkHook so
 * overlay_detail / shotsim_poi calls during think carry the correct
 * bot_owner (they used to always get -1 because g_currentInitBot was
 * only set during brain.open()). */
static int  g_currentInitBot = -1;
static char g_currentInitBrainName[64] = "";

static void preThinkHook(int playerNum) {
    g_currentInitBot = playerNum;  /* -1 signals think complete */
}

/* Keys BrainTest core already binds — bots can't claim these. The
 * check is one-letter-only (uppercase normalized) since shortcuts
 * are single characters. */
static bool isReservedShortcut(char k) {
    /* P is intentionally NOT here — the old host-owned P-window-with-
     * tabs is gone, so bots are free to claim P (e.g. NewAutopilot's
     * Pool breakdown). */
    static const char *reserved = "VMXHFTLGCBQ";
    if (k == '\0') return false;
    char up = (k >= 'a' && k <= 'z') ? (char)(k - 'a' + 'A') : k;
    for (const char *p = reserved; *p; p++) {
        if (up == *p) return true;
    }
    return false;
}

/* Brain → host callback for the panel registry. Auto-namespaces
 * the panel type with the registering bot's brain name so two
 * brains registering "pool_grid" with different schemas don't
 * collide ("NewAutopilot:pool_grid" vs "OtherBot:pool_grid").
 * Per-bot panel modules in brains/<botname>/braintest_panels/
 * register their renderers under the same namespaced names.
 *
 * `shortcut` is optional. When present and not reserved, the panel
 * gets its own SDL window toggled by that key (lazy-created on
 * first toggle). Reserved keys log a warning and are stripped —
 * the panel still registers, just as a tab in the P window. */
static int panelRegisterCallback(const char *name, const char *type,
                                  const char *lua_expr,
                                  const char *shortcut) {
    if (g_currentInitBot < 0) {
        SDL_Log("WARN: braintest_panel_register('%s') called outside "
                "brain.open() — ignored", name ? name : "?");
        return -1;
    }
    char namespaced[PANEL_REG_TYPE_MAX];
    if (type && type[0] && g_currentInitBrainName[0]) {
        SDL_snprintf(namespaced, sizeof(namespaced), "%s:%s",
                     g_currentInitBrainName, type);
    } else if (type && type[0]) {
        SDL_snprintf(namespaced, sizeof(namespaced), "%s", type);
    } else {
        SDL_snprintf(namespaced, sizeof(namespaced), "text");
    }
    /* Validate shortcut. Single ASCII letter only; uppercased; not
     * reserved. Anything else falls back to "no shortcut" with a
     * one-time warning so the panel doesn't silently vanish. */
    char  shortBuf[2] = { 0, 0 };
    const char *useShort = NULL;
    if (shortcut && shortcut[0]) {
        char k = shortcut[0];
        if (k >= 'a' && k <= 'z') k = (char)(k - 'a' + 'A');
        if (k < 'A' || k > 'Z' || shortcut[1] != '\0') {
            SDL_Log("WARN: panel '%s' shortcut '%s' is not a single "
                    "letter — ignored", name, shortcut);
        } else if (isReservedShortcut(k)) {
            SDL_Log("WARN: panel '%s' shortcut '%c' is reserved by "
                    "BrainTest core — falling back to P-window tab",
                    name, k);
        } else {
            shortBuf[0] = k;
            useShort = shortBuf;
        }
    }
    return panelRegistryAdd(g_currentInitBot, name, namespaced,
                            lua_expr, useShort);
}

/* braintest_shotsim_poi_register host hook. Routes Lua → POI
 * registry; returns the slot index or -1. The brain's bot_owner
 * is the same as g_currentInitBot used for panel registration. */
/* viz_detail registry adapters. The brain calls overlay_detail /
 * overlay_detail_text / overlay_detail_clear via braincore.c; these
 * translate the string-kind to the VizDetailKind enum and forward to
 * the per-tick registry. bot_owner is taken from g_currentInitBot
 * during brain.open and from the followed bot otherwise (-1 if no
 * bot context is active). */
static VizDetailKind vizDetailKindFromStr(const char *s) {
    if (!s) return VIZDETAIL_KIND_RECT;
    if (strcmp(s, "rect")   == 0) return VIZDETAIL_KIND_RECT;
    if (strcmp(s, "circle") == 0) return VIZDETAIL_KIND_CIRCLE;
    if (strcmp(s, "text")   == 0) return VIZDETAIL_KIND_TEXT;
    return VIZDETAIL_KIND_RECT;
}
static int vizDetailRegisterCallback(const char *id, const char *kind,
                                      float x1, float y1, float x2, float y2,
                                      const char *label) {
    return vizDetailRegister(g_currentInitBot, id, vizDetailKindFromStr(kind),
                              x1, y1, x2, y2, label);
}
static int vizDetailAppendBodyCallback(const char *id, const char *line) {
    return vizDetailAppendBody(g_currentInitBot, id, line);
}
static void vizDetailClearCallback(void) {
    vizDetailRegistryClear();
}

static void pillContribClearCallback(void) {
    pillContribClear();
}
static int pillContribBeginPillCallback(int pill_id, int mx, int my) {
    return pillContribBeginPill(pill_id, mx, my);
}
static void pillContribAddTileCallback(int slot, int tx, int ty, float value) {
    pillContribAddTile(slot, tx, ty, value);
}

static int shotSimPoiRegisterCallback(const char *name,
                                       const char *lua_expr) {
    if (g_currentInitBot < 0) {
        SDL_Log("WARN: braintest_shotsim_poi_register('%s') called "
                "outside brain.open() — ignored", name ? name : "?");
        return -1;
    }
    return shotSimPoiRegister(g_currentInitBot, name, lua_expr);
}

/* The panel window's poll callback runs from inside ImGui rendering
 * and only knows the panel index — it has no app handle. Stash the
 * followBot + recording-enabled flag + sim tick in file-statics the
 * callback can read. Updated every frame the panel window is visible. */
static BYTE     g_panelPollFollowBot   = 0;
/* Off by default: recording produces one file per polled tick per
 * panel, which adds up fast over long sessions and has no rotation
 * yet. Opt in with --record-panels when you want offline replay. */
static bool     g_panelRecordEnabled   = false;
static char     g_panelRecordDir[FILENAME_MAX] = "";
static uint32_t g_panelPollTick        = 0;
/* Playback bridge — when set, the poll callback returns the
 * recorded panel JSON for `g_panelPollFrame` instead of asking
 * the live brain. Updated by appRender each frame. */
static RecordingBuffer *g_panelPollRecording = NULL;
static int              g_panelPollFrame    = -1;

/* Slugify a panel name for the filename: lowercase, non-alnum → '_'.
 * Output buffer must be ≥ strlen(in)+1. */
static void slugifyPanelName(const char *in, char *out, size_t outLen) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < outLen; i++) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (c >= 'A' && c <= 'Z') {
            out[j++] = (char)(c - 'A' + 'a');
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

static char *panelPollCallback(int panel_idx) {
    const PanelRegistryEntry *e = panelRegistryGet(panel_idx);
    if (!e || !e->lua_expr[0]) return NULL;
    /* Playback path: serve the recorded JSON for this frame. We
     * hand back a heap-allocated copy because the panel window
     * frees what we return. NULL → renderer keeps showing whatever
     * it already had (better than blanking on a gap). */
    if (g_panelPollRecording
        && g_panelPollFrame >= 0
        && g_panelPollFrame < g_panelPollRecording->count
        && panel_idx >= 0
        && panel_idx < PANEL_REG_MAX) {
        const char *rec = g_panelPollRecording->frames[g_panelPollFrame]
                          .recordedPanels[panel_idx];
        if (!rec) return NULL;
        size_t n = strlen(rec);
        char *copy = (char *)malloc(n + 1);
        if (copy) memcpy(copy, rec, n + 1);
        return copy;
    }
    char *body = botManagerEvalLuaString(g_panelPollFollowBot, e->lua_expr);
    /* Persist to disk so the same per-tick snapshot can be inspected
     * offline (great for "look at the queue at tick 4321" — files
     * are JSON for typed panels, raw text for the "text" type). */
    if (body && g_panelRecordEnabled && g_panelRecordDir[0]) {
        char slug[PANEL_REG_NAME_MAX];
        slugifyPanelName(e->name, slug, sizeof(slug));
        char path[FILENAME_MAX];
        SDL_snprintf(path, sizeof(path), "%s/p%d_%s_%u.json",
                     g_panelRecordDir, e->bot_owner, slug,
                     (unsigned)g_panelPollTick);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(body, 1, strlen(body), f);
            fclose(f);
        }
    }
    return body;
}

static void signalHandler(int sig) {
    (void)sig;
    appQuit = TRUE;
}

/* ------------------------------------------------------------------ */
/* Command-line parsing                                                */
/* ------------------------------------------------------------------ */

static char optBrain[512] = "brains/NewAutopilot";
static char optMap[512]   = "";
static int  optNumPlayers = 1;
static int  optFollow     = 0;
static aiType  optAI      = aiFull;
static gameType optGame   = gameOpen;

static void printUsage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -brain PATH      Brain script directory (default: brains/NewAutopilot)\n"
        "  -noplayers N     Number of bot players (default: 1)\n"
        "  -map PATH        Map file (default: built-in Everard Island)\n"
        "  -follow N        Follow bot N with camera (default: 0)\n"
        "  -ai TYPE         AI type: none, yes, advantage, full (default: full)\n"
        "  -game TYPE       Game type: open, tournament, strict (default: open)\n"
        "  --record-panels      Write per-tick panel JSON to debug_sessions/<ts>/panels/\n"
        "\n"
        "Controls:\n"
        "  Arrows           Scroll map (switches to free camera)\n"
        "  Space            Pause / unpause\n"
        "  Tab              Cycle followed bot\n"
        "  1-4              Toggle overlays (influence/danger/frontline/path)\n"
        "  8                Toggle strategic pill-placement heatmap\n"
        "  0                All overlays off\n"
        "  +/-/scroll       Zoom in/out\n"
        "  F                Toggle free camera / follow mode\n"
        "  Left click       Show A* cost + path to tile (magenta)\n"
        "  Right click      Clear click overlay\n",
        prog);
}

static bool parseArgs(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-brain") == 0 || strcmp(argv[i], "--brain") == 0) && i + 1 < argc) {
            strncpy(optBrain, argv[++i], sizeof(optBrain) - 1);
        } else if ((strcmp(argv[i], "-noplayers") == 0 || strcmp(argv[i], "--noplayers") == 0) && i + 1 < argc) {
            optNumPlayers = atoi(argv[++i]);
            if (optNumPlayers < 1) optNumPlayers = 1;
            if (optNumPlayers > 16) optNumPlayers = 16;
        } else if ((strcmp(argv[i], "-map") == 0 || strcmp(argv[i], "--map") == 0) && i + 1 < argc) {
            strncpy(optMap, argv[++i], sizeof(optMap) - 1);
        } else if ((strcmp(argv[i], "-follow") == 0 || strcmp(argv[i], "--follow") == 0) && i + 1 < argc) {
            optFollow = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "-ai") == 0 || strcmp(argv[i], "--ai") == 0) && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "none") == 0)           optAI = aiNone;
            else if (strcmp(v, "yes") == 0)        optAI = aiYes;
            else if (strcmp(v, "advantage") == 0)  optAI = aiYesAdvantage;
            else if (strcmp(v, "full") == 0)        optAI = aiFull;
            else { fprintf(stderr, "Unknown AI type: %s\n", v); return FALSE; }
        } else if ((strcmp(argv[i], "-game") == 0 || strcmp(argv[i], "--game") == 0) && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "open") == 0)             optGame = gameOpen;
            else if (strcmp(v, "tournament") == 0)  optGame = gameTournament;
            else if (strcmp(v, "strict") == 0)      optGame = gameStrictTournament;
            else { fprintf(stderr, "Unknown game type: %s\n", v); return FALSE; }
        } else if (strcmp(argv[i], "--record-panels") == 0) {
            /* Enable per-tick panel JSON dumps to
             * debug_sessions/<ts>/panels/. Off by default — there's
             * no rotation yet so a long session creates many files.
             * Use when you want to inspect / replay later. */
            g_panelRecordEnabled = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
            return FALSE;
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Find brain script                                                   */
/* ------------------------------------------------------------------ */

static bool findBrainScript(const char *base, char *out, size_t outLen) {
    /* Try: base/init.lua, base.lua, base as-is */
    char tryPath[1024];

    SDL_snprintf(tryPath, sizeof(tryPath), "%s/init.lua", base);
    SDL_IOStream *io = SDL_IOFromFile(tryPath, "r");
    if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", tryPath); return true; }

    SDL_snprintf(tryPath, sizeof(tryPath), "%s.lua", base);
    io = SDL_IOFromFile(tryPath, "r");
    if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", tryPath); return true; }

    io = SDL_IOFromFile(base, "r");
    if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", base); return true; }

    /* Try Brains/ prefix variants (including ../brains/ for build dirs) */
    const char *prefixes[] = { "Brains/", "brains/", "../brains/", "data/Brains/" };
    for (int i = 0; i < 4; i++) {
        SDL_snprintf(tryPath, sizeof(tryPath), "%s%s/init.lua", prefixes[i], base);
        io = SDL_IOFromFile(tryPath, "r");
        if (io) { SDL_CloseIO(io); SDL_snprintf(out, outLen, "%s", tryPath); return true; }
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* Map bounds calculation                                              */
/* ------------------------------------------------------------------ */

static void calcMapBounds(BrainTestApp *app) {
    GameSim *gs = &app->sim.sim;
    int minX = 255, minY = 255, maxX = 0, maxY = 0;

    for (int y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (int x = 0; x < MAP_ARRAY_SIZE; x++) {
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) != DEEP_SEA) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    BYTE np = pillsGetNumPills(&gs->pb);
    for (BYTE i = 1; i <= np; i++) {
        pillbox p; pillsGetPill(&gs->pb, &p, i);
        if (p.x < minX) minX = p.x; if (p.x > maxX) maxX = p.x;
        if (p.y < minY) minY = p.y; if (p.y > maxY) maxY = p.y;
    }
    BYTE nb = basesGetNumBases(&gs->bs);
    for (BYTE i = 1; i <= nb; i++) {
        base b; basesGetBase(&gs->bs, &b, i);
        if (b.x < minX) minX = b.x; if (b.x > maxX) maxX = b.x;
        if (b.y < minY) minY = b.y; if (b.y > maxY) maxY = b.y;
    }
    BYTE ns = startsGetNumStarts(&gs->ss);
    for (BYTE i = 1; i <= ns; i++) {
        start st; startsGetStartStruct(&gs->ss, &st, i);
        if (st.x < minX) minX = st.x; if (st.x > maxX) maxX = st.x;
        if (st.y < minY) minY = st.y; if (st.y > maxY) maxY = st.y;
    }

    int pad = 5;
    if (minX > pad) minX -= pad; else minX = 0;
    if (minY > pad) minY -= pad; else minY = 0;
    if (maxX + pad < 255) maxX += pad; else maxX = 255;
    if (maxY + pad < 255) maxY += pad; else maxY = 255;

    app->mapMinX = minX; app->mapMinY = minY;
    app->mapMaxX = maxX; app->mapMaxY = maxY;
}

/* ------------------------------------------------------------------ */
/* Screen ↔ map coordinate conversion                                  */
/* ------------------------------------------------------------------ */

static bool screenToMap(BrainTestApp *app, float sx, float sy,
                        int screenW, int screenH, int *outMX, int *outMY) {
    int zf = app->zoomFactor;
    int tilePixels = 16 * zf;
    int centerMX = app->viewCenterX >> 8;
    int centerMY = app->viewCenterY >> 8;
    float scx = screenW / 2.0f;
    float scy = screenH / 2.0f;
    float mapOriginX = scx - centerMX * tilePixels
                       - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
    float mapOriginY = scy - centerMY * tilePixels
                       - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;

    int mx = (int)floorf((sx - mapOriginX) / tilePixels);
    int my = (int)floorf((sy - mapOriginY) / tilePixels);
    if (mx < 0 || mx > 255 || my < 0 || my > 255) return false;
    *outMX = mx;
    *outMY = my;
    return true;
}

/* Screen → world units (full 1/256-tile precision). Used for the
 * sub-tile hover-coords HUD overlay. Returns false if the cursor
 * is outside the 256×256-tile map. */
static bool screenToWU(BrainTestApp *app, float sx, float sy,
                       int screenW, int screenH,
                       int *outWX, int *outWY) {
    int zf = app->zoomFactor;
    int tilePixels = 16 * zf;
    int centerMX = app->viewCenterX >> 8;
    int centerMY = app->viewCenterY >> 8;
    float scx = screenW / 2.0f;
    float scy = screenH / 2.0f;
    float mapOriginX = scx - centerMX * tilePixels
                       - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
    float mapOriginY = scy - centerMY * tilePixels
                       - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;
    float tileFX = (sx - mapOriginX) / tilePixels;
    float tileFY = (sy - mapOriginY) / tilePixels;
    int wx = (int)floorf(tileFX * 256.0f);
    int wy = (int)floorf(tileFY * 256.0f);
    if (wx < 0 || wx > 256 * 256 - 1 || wy < 0 || wy > 256 * 256 - 1)
        return false;
    *outWX = wx;
    *outWY = wy;
    return true;
}

/* ------------------------------------------------------------------ */
/* Debug pathfinder sync & click-cost computation                      */
/* ------------------------------------------------------------------ */

/* Copy terrain/danger/influence/config from brain's pathfinder to debug PF */
static void syncDebugPathfinder(BrainTestApp *app) {
    BrainPathfinder *src = botManagerGetBrainPathfinder(app->followBot);
    BrainPathfinder *dst = app->debugPF;
    if (!src || !dst) return;

    dst->map = src->map;
    memcpy(dst->danger_grid, src->danger_grid, sizeof(src->danger_grid));
    memcpy(dst->influence_grid, src->influence_grid, sizeof(src->influence_grid));
    memcpy(dst->overlay_grid, src->overlay_grid, sizeof(src->overlay_grid));
    memcpy(dst->terrain_cost_table, src->terrain_cost_table, sizeof(src->terrain_cost_table));
    memcpy(dst->terrain_cost_boat_table, src->terrain_cost_boat_table, sizeof(src->terrain_cost_boat_table));
    memcpy(dst->terrain_speed_table, src->terrain_speed_table, sizeof(src->terrain_speed_table));
    dst->turn_cost = src->turn_cost;
    dst->wall_shoot_cost = src->wall_shoot_cost;
    dst->wall_shoot_shells = src->wall_shoot_shells;
    dst->shell_reserve = src->shell_reserve;
    dst->road_build_cost = src->road_build_cost;
    dst->tree_reserve = src->tree_reserve;
    dst->mine_penalty = src->mine_penalty;
    dst->estimate_samples = src->estimate_samples;
    dst->water_drain_rate = src->water_drain_rate;
    dst->shell_loss_cost = src->shell_loss_cost;
    dst->mine_loss_cost = src->mine_loss_cost;
    dst->armour_drain_rate = src->armour_drain_rate;
    dst->min_shells = src->min_shells;
    dst->min_mines = src->min_mines;
    dst->min_armour = src->min_armour;
}

/* Run a full A* from tank to (dmx,dmy) on the debug pathfinder */
static void computeClickPath(BrainTestApp *app, int dmx, int dmy) {
    BrainPathfinder *dpf = app->debugPF;
    if (!dpf || !dpf->map) return;

    /* Origin: in playback, use the followed tank's RECORDED position
     * for that frame so the A* path lines up with where the tank is
     * being rendered. In live mode use the live tank state. Without
     * this, a playback click would show a path from the live tank
     * (which has moved on by many ticks), not the one on screen. */
    int smx, smy, in_boat = 0;
    bool got_origin = false;
    if (app->playbackMode
        && app->playbackFrame >= 0
        && app->playbackFrame < app->recording.count) {
        const RecordingFrame *pf_ = &app->recording.frames[app->playbackFrame];
        for (int i = 0; i < pf_->tankCount; i++) {
            if (pf_->tanks[i].playerNum == app->followBot) {
                smx = pf_->tanks[i].worldX >> 8;
                smy = pf_->tanks[i].worldY >> 8;
                in_boat = (pf_->tanks[i].tankStatus & 0x0F) ? 1 : 0;
                got_origin = true;
                break;
            }
        }
    }
    if (!got_origin) {
        WORLD twx, twy;
        if (!serverSimGetTankState(&app->sim, app->followBot, &twx, &twy)) return;
        smx = twx >> 8;
        smy = twy >> 8;
        in_boat = (app->sim.sim.tanks[app->followBot] != NULL &&
                   tankIsOnBoat(&app->sim.sim.tanks[app->followBot])) ? 1 : 0;
    }

    /* Reset the debug PF search state by changing destination */
    dpf->status = 0;
    dpf->dest_x = -1;
    dpf->dest_y = -1;

    /* Run A* to completion with a generous budget */
    int nx, ny;
    int status = 0;
    for (int iter = 0; iter < 20 && status == 0; iter++) {
        status = brainPathfinderPathTo(dpf, smx, smy, dmx, dmy,
                                        in_boat, 40, 20, 20, 40,
                                        100000, &nx, &ny);
    }

    /* A* cost: real path cost if A* completed, sentinel otherwise so
     * the HUD can render "unreached" without conflating with the
     * estimate. */
    app->clickCost = (status == 1)
        ? dpf->g_cost[dmy * 256 + dmx]
        : 1e30f;
    app->clickEstCost = brainPathfinderEstimateCost(dpf, smx, smy, dmx, dmy, in_boat);

    /* Trace the path */
    if (status == 1) {
        app->clickPathLen = brainPathfinderTracePath(dpf,
            app->clickPath_x, app->clickPath_y, 2048);
    } else {
        app->clickPathLen = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Debug overlay rendering                                             */
/* ------------------------------------------------------------------ */

/* RGBA pixel helpers */
static inline void setPixel(uint8_t *pixels, int pitch, int x, int y,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t *p = pixels + y * pitch + x * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static inline void blendPixel(uint8_t *pixels, int pitch, int x, int y,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t *p = pixels + y * pitch + x * 4;
    /* Simple additive alpha: keep max alpha, blend color */
    uint8_t oldA = p[3];
    if (a > oldA) {
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    } else if (oldA > 0) {
        /* Blend towards new color */
        int blend = a * 255 / (oldA + a);
        p[0] = (uint8_t)(p[0] + (r - p[0]) * blend / 255);
        p[1] = (uint8_t)(p[1] + (g - p[1]) * blend / 255);
        p[2] = (uint8_t)(p[2] + (b - p[2]) * blend / 255);
        p[3] = (uint8_t)(oldA + (255 - oldA) * a / 255);
    }
}

static void updateOverlayTexture(BrainTestApp *app) {
    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (!pf) return;
    if (!vizFlag(app->regIdxInfluence) && !vizFlag(app->regIdxDanger) && !vizFlag(app->regIdxFrontLine)) return;

    /* Cache key: live ticks count up from 0; playback frames live in
     * the high half so a scrub never collides with a live tick. Without
     * this, scrubbing while the live sim is paused would re-use the
     * stale texture (sim.tick frozen) even though the playback patch-in
     * just swapped pf->danger_grid to a different historical state. */
    uint32_t cacheKey = app->playbackMode
        ? (0x80000000u | (uint32_t)app->playbackFrame)
        : app->sim.tick;
    if (cacheKey == app->overlayTick && !app->overlayDirty) return;
    app->overlayTick = cacheKey;
    app->overlayDirty = false;

    /* Lock the 256x256 overlay texture */
    SDL_Surface *surf = NULL;
    if (!SDL_LockTextureToSurface(app->overlayTex, NULL, &surf)) return;

    uint8_t *pixels = (uint8_t *)surf->pixels;
    int pitch = surf->pitch;

    /* Clear to transparent */
    memset(pixels, 0, 256 * pitch);

    /* Influence overlay: blue (friendly +) / red (hostile -) */
    if (vizFlag(app->regIdxInfluence)) {
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                int16_t inf = brainPathfinderInfluenceAt(pf, x, y);
                if (inf == 0) continue;
                if (inf > 0) {
                    /* Friendly = blue, intensity proportional to value */
                    int v = inf > 100 ? 100 : inf;
                    uint8_t a = (uint8_t)(60 + v * 160 / 100);
                    blendPixel(pixels, pitch, x, y, 40, 80, 255, a);
                } else {
                    /* Hostile = red */
                    int v = -inf > 100 ? 100 : -inf;
                    uint8_t a = (uint8_t)(60 + v * 160 / 100);
                    blendPixel(pixels, pitch, x, y, 255, 40, 40, a);
                }
            }
        }
    }

    /* Danger overlay: red/orange intensity */
    if (vizFlag(app->regIdxDanger)) {
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                float d = brainPathfinderDangerAt(pf, x, y);
                if (d <= 0.0f) continue;
                int v = (int)(d * 8.0f);
                if (v > 200) v = 200;
                uint8_t a = (uint8_t)(60 + v * 180 / 200);
                blendPixel(pixels, pitch, x, y, 255, 120, 0, a);
            }
        }
    }

    /* Front line overlay: bright yellow points */
    if (vizFlag(app->regIdxFrontLine)) {
        int flx[4096], fly[4096];
        int n = brainPathfinderFindFrontLine(pf, flx, fly, 4096);
        for (int i = 0; i < n; i++) {
            if (flx[i] >= 0 && flx[i] < 256 && fly[i] >= 0 && fly[i] < 256) {
                setPixel(pixels, pitch, flx[i], fly[i], 255, 255, 0, 220);
            }
        }
    }

    SDL_UnlockTexture(app->overlayTex);
}

/* Cache the brain's A* path for screen-space rendering */
static void updateCachedPath(BrainTestApp *app) {
    if (!vizFlag(app->regIdxPath)) return;
    /* In playback the path was already patched in from the recorded
     * frame; re-tracing the live brain's slate would clobber it. */
    if (app->playbackMode) return;
    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (!pf) return;

    /* The brain mostly navigates by Dijkstra slate lookup now —
     * `cpf.path_to` tries Dijkstra first and falls back to A* only
     * when no slate is available. Mirror that priority here so the
     * green polyline reflects what the brain is actually following.
     *
     * To trace a Dijkstra path we need the destination tile. The
     * brain's goal info carries it (`mx, my` on the active goal).
     * If goal info isn't available, fall through to the legacy A*
     * trace which uses pf->dest_x/y from the last A* search. */
    int n = 0;
    BrainGoalInfo gi;
    if (botManagerGetGoalInfo(app->followBot, &gi)
        && gi.kind[0] != '\0'
        && strcmp(gi.kind, "none") != 0) {
        /* Ask the pathfinder which slate currently holds the freshest
         * KIND_NORMAL search. The slate index isn't stable — the
         * brain rotates them via DijkstraPickReuseSlate — so a
         * hardcoded `0` would silently go stale whenever the brain
         * parked NORMAL elsewhere. */
        n = brainPathfinderDijkstraTracePathByKind(pf, /* KIND_NORMAL */ 0,
                                                     gi.mx, gi.my,
                                                     app->cachedPath_x,
                                                     app->cachedPath_y, 2048);
    }
    if (n == 0) {
        n = brainPathfinderTracePath(pf, app->cachedPath_x,
                                      app->cachedPath_y, 2048);
    }
    if (n > 0) {
        app->cachedPathLen = n;
    } else {
        /* No path either way — clear the previous trace so we don't
         * keep drawing a stale polyline indefinitely. */
        app->cachedPathLen = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Render the debug overlay on top of the map                          */
/* ------------------------------------------------------------------ */

/* Convert map tile coords to screen center of that tile */
static void mapToScreen(BrainTestApp *app, int mx, int my,
                         int screenW, int screenH,
                         float *outSX, float *outSY) {
    int zf = app->zoomFactor;
    int tilePixels = 16 * zf;
    int centerMX = app->viewCenterX >> 8;
    int centerMY = app->viewCenterY >> 8;
    float scx = screenW / 2.0f;
    float scy = screenH / 2.0f;
    float mapOriginX = scx - centerMX * tilePixels
                       - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
    float mapOriginY = scy - centerMY * tilePixels
                       - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;
    *outSX = mapOriginX + ((float)mx + 0.5f) * tilePixels;
    *outSY = mapOriginY + ((float)my + 0.5f) * tilePixels;
}

/* Draw a path as connected screen-space lines */
static void renderPathLines(BrainTestApp *app, int screenW, int screenH,
                             int *path_x, int *path_y, int pathLen,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (pathLen < 2) return;
    SDL_SetRenderDrawColor(app->renderer, r, g, b, a);
    for (int i = 0; i < pathLen - 1; i++) {
        float x1, y1, x2, y2;
        mapToScreen(app, path_x[i], path_y[i], screenW, screenH, &x1, &y1);
        mapToScreen(app, path_x[i+1], path_y[i+1], screenW, screenH, &x2, &y2);
        SDL_RenderLine(app->renderer, x1, y1, x2, y2);
    }
}

static void renderOverlay(BrainTestApp *app, int screenW, int screenH) {
    /* Update cached path data (independent of grid overlays) */
    updateCachedPath(app);

    bool hasGridOverlay = vizFlag(app->regIdxInfluence) || vizFlag(app->regIdxDanger) || vizFlag(app->regIdxFrontLine);
    bool hasPathOverlay = (vizFlag(app->regIdxPath) && app->cachedPathLen > 0) ||
                          (app->clickActive && app->clickPathLen > 0);

    if (!hasGridOverlay && !hasPathOverlay) return;

    /* Grid overlays (influence/danger/frontline) via texture */
    if (hasGridOverlay) {
        updateOverlayTexture(app);

        int zf = app->zoomFactor;
        int tilePixels = 16 * zf;
        int centerMX = app->viewCenterX >> 8;
        int centerMY = app->viewCenterY >> 8;
        float scx = screenW / 2.0f;
        float scy = screenH / 2.0f;
        float mapOriginX = scx - centerMX * tilePixels
                           - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
        float mapOriginY = scy - centerMY * tilePixels
                           - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;

        SDL_FRect destRect = {
            mapOriginX, mapOriginY,
            256.0f * tilePixels, 256.0f * tilePixels
        };

        SDL_SetTextureBlendMode(app->overlayTex, SDL_BLENDMODE_BLEND);
        SDL_RenderTexture(app->renderer, app->overlayTex, NULL, &destRect);
    }

    /* Path overlays as screen-space lines */
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

    if (vizFlag(app->regIdxPath) && app->cachedPathLen > 0) {
        renderPathLines(app, screenW, screenH,
                        app->cachedPath_x, app->cachedPath_y, app->cachedPathLen,
                        0, 255, 80, 240);
    }

    if (app->clickActive && app->clickPathLen > 0) {
        renderPathLines(app, screenW, screenH,
                        app->clickPath_x, app->clickPath_y, app->clickPathLen,
                        255, 100, 255, 220);
        /* Click destination marker — small cross */
        float cx, cy;
        mapToScreen(app, app->clickMX, app->clickMY, screenW, screenH, &cx, &cy);
        SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
        SDL_RenderLine(app->renderer, cx - 4, cy, cx + 4, cy);
        SDL_RenderLine(app->renderer, cx, cy - 4, cx, cy + 4);
    }
}

/* Build the Lua statement that mirrors the registry's state into
 * the bot's Lua globals: _BT_VIZ_<UPPER_ID> per-id booleans plus
 * _BT_VIZ_IDS = {id=idx, ...} lookup. Pushed every brain frame so
 * a freshly-spawned bot sees current state on its first think. */
static void pushVizStateToBots(bool vizSuppressActive) {
    char buf[16384];
    int  off = 0;
    int  n = vizRegistryCount();
    for (int i = 0; i < n; i++) {
        const VizRegistryEntry *e = vizRegistryGet(i);
        if (!e || !e->id[0]) continue;
        char up[VIZ_REG_ID_MAX];
        size_t L = strlen(e->id);
        if (L >= sizeof(up)) L = sizeof(up) - 1;
        for (size_t j = 0; j < L; j++) {
            char c = e->id[j];
            up[j] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        up[L] = '\0';
        int w = SDL_snprintf(buf + off, sizeof(buf) - off,
                              "_G._BT_VIZ_%s=%s; ",
                              up, e->is_on ? "true" : "false");
        if (w < 0 || w >= (int)(sizeof(buf) - off)) break;
        off += w;
    }
    /* IDS lookup. */
    {
        int w = SDL_snprintf(buf + off, sizeof(buf) - off,
                              "_G._BT_VIZ_IDS={");
        if (w > 0 && w < (int)(sizeof(buf) - off)) off += w;
        for (int i = 0; i < n; i++) {
            const VizRegistryEntry *e = vizRegistryGet(i);
            if (!e || !e->id[0]) continue;
            w = SDL_snprintf(buf + off, sizeof(buf) - off,
                              "%s=%d,", e->id, i);
            if (w < 0 || w >= (int)(sizeof(buf) - off)) break;
            off += w;
        }
        w = SDL_snprintf(buf + off, sizeof(buf) - off, "}; ");
        if (w > 0 && w < (int)(sizeof(buf) - off)) off += w;
    }
    /* Suppress flag. */
    {
        int w = SDL_snprintf(buf + off, sizeof(buf) - off,
                              "_G._BT_VIZ_SUPPRESS_ALL=%s; ",
                              vizSuppressActive ? "true" : "false");
        if (w > 0 && w < (int)(sizeof(buf) - off)) off += w;
    }
    if (off == 0) return;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (botManagerIsBot((BYTE)i)) botManagerExecLua((BYTE)i, buf);
    }
}

/* Per-tick recording capture. Called once per game tick from the
 * main loop after botManagerTick + serverSimTick. Captures the
 * followed bot's view of the world (map, grids, brainMap), the
 * authoritative tank/shell/base/pill state from the server sim,
 * and the brain's per-panel JSON output (so playback can show
 * the same pool breakdown / queue text the user would have seen
 * at that tick). */
static void recordingCapture(BrainTestApp *app) {
    RecordingBuffer *rb = &app->recording;
    if (rb->count >= MAX_RECORDING_FRAMES) return;
    if (rb->count >= rb->capacity) {
        int newCap = rb->capacity == 0 ? 1024 : rb->capacity * 2;
        if (newCap > MAX_RECORDING_FRAMES) newCap = MAX_RECORDING_FRAMES;
        rb->frames = (RecordingFrame *)realloc(rb->frames,
                       newCap * sizeof(RecordingFrame));
        rb->capacity = newCap;
    }

    RecordingFrame *f = &rb->frames[rb->count];
    memset(f, 0, sizeof(*f));
    f->tick = app->sim.tick;

    int mapSz  = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
    int gridSz = 65536;
    bool needKeyframe = !rb->hasPrev || (rb->count % KEYFRAME_INTERVAL == 0);
    f->isKeyframe = needKeyframe;

    /* ── Map terrain ── */
    const BYTE *curMap = &app->sim.sim.mp->mapItem[0][0];
    if (needKeyframe) {
        f->fullMap = (BYTE *)malloc(mapSz);
        memcpy(f->fullMap, curMap, mapSz);
    } else {
        ByteDelta tmp[4096];
        int n = 0;
        for (int i = 0; i < mapSz && n < 4096; i++) {
            if (curMap[i] != rb->prevMap[i]) {
                tmp[n].offset = i;
                tmp[n].oldVal = rb->prevMap[i];
                tmp[n].newVal = curMap[i];
                n++;
            }
        }
        f->mapDeltaCount = n;
        if (n > 0) {
            f->mapDeltas = (ByteDelta *)malloc(n * sizeof(ByteDelta));
            memcpy(f->mapDeltas, tmp, n * sizeof(ByteDelta));
        }
    }
    memcpy(rb->prevMap, curMap, mapSz);

    /* ── Pathfinder grids (followed bot) ── */
    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (pf) {
        if (needKeyframe) {
            f->fullDanger = (uint16_t *)malloc(gridSz * sizeof(uint16_t));
            memcpy(f->fullDanger, pf->danger_grid, gridSz * sizeof(uint16_t));
            f->fullInfluence = (int16_t *)malloc(gridSz * sizeof(int16_t));
            memcpy(f->fullInfluence, pf->influence_grid, gridSz * sizeof(int16_t));
            f->fullOverlay = (uint16_t *)malloc(gridSz * sizeof(uint16_t));
            memcpy(f->fullOverlay, pf->overlay_grid, gridSz * sizeof(uint16_t));
        } else {
            GridDelta tmp[4096];
            int n;
            n = 0;
            for (int i = 0; i < gridSz && n < 4096; i++) {
                if (pf->danger_grid[i] != rb->prevDanger[i]) {
                    tmp[n].offset = i;
                    tmp[n].oldVal = (int16_t)rb->prevDanger[i];
                    tmp[n].newVal = (int16_t)pf->danger_grid[i];
                    n++;
                }
            }
            f->dangerDeltaCount = n;
            if (n > 0) {
                f->dangerDeltas = (GridDelta *)malloc(n * sizeof(GridDelta));
                memcpy(f->dangerDeltas, tmp, n * sizeof(GridDelta));
            }
            n = 0;
            for (int i = 0; i < gridSz && n < 4096; i++) {
                if (pf->influence_grid[i] != rb->prevInfluence[i]) {
                    tmp[n].offset = i;
                    tmp[n].oldVal = rb->prevInfluence[i];
                    tmp[n].newVal = pf->influence_grid[i];
                    n++;
                }
            }
            f->influenceDeltaCount = n;
            if (n > 0) {
                f->influenceDeltas = (GridDelta *)malloc(n * sizeof(GridDelta));
                memcpy(f->influenceDeltas, tmp, n * sizeof(GridDelta));
            }
            n = 0;
            for (int i = 0; i < gridSz && n < 4096; i++) {
                if (pf->overlay_grid[i] != rb->prevOverlay[i]) {
                    tmp[n].offset = i;
                    tmp[n].oldVal = (int16_t)rb->prevOverlay[i];
                    tmp[n].newVal = (int16_t)pf->overlay_grid[i];
                    n++;
                }
            }
            f->overlayDeltaCount = n;
            if (n > 0) {
                f->overlayDeltas = (GridDelta *)malloc(n * sizeof(GridDelta));
                memcpy(f->overlayDeltas, tmp, n * sizeof(GridDelta));
            }
        }
        memcpy(rb->prevDanger,    pf->danger_grid,    gridSz * sizeof(uint16_t));
        memcpy(rb->prevInfluence, pf->influence_grid, gridSz * sizeof(int16_t));
        memcpy(rb->prevOverlay,   pf->overlay_grid,   gridSz * sizeof(uint16_t));
    }

    /* ── brainMap (fog of war) for followed bot. The pathfinder's
     *    `map` field already points at the bot's brainMap (see
     *    brainPathfinderSetMap), so we read it through there
     *    instead of needing botManagerGetClientSim. ── */
    if (pf && pf->map) {
        const BYTE *bm = pf->map;
        if (needKeyframe) {
            f->fullBrainMap = (BYTE *)malloc(mapSz);
            memcpy(f->fullBrainMap, bm, mapSz);
        } else {
            ByteDelta tmp[4096];
            int n = 0;
            for (int i = 0; i < mapSz && n < 4096; i++) {
                if (bm[i] != rb->prevBrainMap[i]) {
                    tmp[n].offset = i;
                    tmp[n].oldVal = rb->prevBrainMap[i];
                    tmp[n].newVal = bm[i];
                    n++;
                }
            }
            f->brainMapDeltaCount = n;
            if (n > 0) {
                f->brainMapDeltas = (ByteDelta *)malloc(n * sizeof(ByteDelta));
                memcpy(f->brainMapDeltas, tmp, n * sizeof(ByteDelta));
            }
        }
        memcpy(rb->prevBrainMap, bm, mapSz);
    }
    rb->hasPrev = true;

    /* ── Tanks / shells / explosions via the existing snapshot path ── */
    SnapshotHeader hdr;
    GameEvent      evtBuf[MAX_SNAPSHOT_EVENTS];
    BaseSnapshot   dummyB[1];
    PillSnapshot   dummyP[1];
    TkExplosionSnapshot dummyTkExp[1];
    /* Recording is god-view: capture every tank/shell regardless of how
     * far they are from followBot. Without this, tanks that drift outside
     * followBot's snapshot viewport vanish from playback. */
    serverSimBuildSnapshot(&app->sim, app->followBot, &hdr,
                           f->tanks, MAX_TANKS,
                           f->snapShells, MAX_SNAPSHOT_SHELLS,
                           dummyTkExp, 0,
                           dummyB, 0,
                           dummyP, 0,
                           evtBuf, MAX_SNAPSHOT_EVENTS,
                           true);
    f->tankCount  = hdr.tankCount;
    f->shellCount = hdr.shellCount;

    /* ── Bases / pills (read directly from the server sim) ── */
    f->baseCount = app->sim.sim.bs->numBases;
    if (f->baseCount > MAX_SNAPSHOT_BASES) f->baseCount = MAX_SNAPSHOT_BASES;
    for (int i = 0; i < f->baseCount; i++) {
        f->snapBases[i].owner  = app->sim.sim.bs->item[i].owner;
        f->snapBases[i].armour = app->sim.sim.bs->item[i].armour;
        f->snapBases[i].shells = app->sim.sim.bs->item[i].shells;
        f->snapBases[i].mines  = app->sim.sim.bs->item[i].mines;
    }
    f->pillCount = app->sim.sim.pb->numPills;
    if (f->pillCount > MAX_SNAPSHOT_PILLS) f->pillCount = MAX_SNAPSHOT_PILLS;
    for (int i = 0; i < f->pillCount; i++) {
        f->snapPills[i].x      = app->sim.sim.pb->item[i].x;
        f->snapPills[i].y      = app->sim.sim.pb->item[i].y;
        f->snapPills[i].owner  = app->sim.sim.pb->item[i].owner;
        f->snapPills[i].armour = app->sim.sim.pb->item[i].armour;
        f->snapPills[i].speed  = app->sim.sim.pb->item[i].speed;
        f->snapPills[i].inTank = app->sim.sim.pb->item[i].inTank ? 1 : 0;
    }

    /* ── Camera + brain perf for HUD ── */
    f->viewCenterX = app->viewCenterX;
    f->viewCenterY = app->viewCenterY;
    f->thinkMs     = (float)botManagerGetLastThinkMs(app->followBot);

    /* ── Goal info (drives scrubber goal-change tick markers) ── */
    if (botManagerGetGoalInfo(app->followBot, &f->goalInfo)) {
        f->goalInfoValid = true;
    }

    /* ── Per-bot overlay command snapshots ── copy each active
     * bot's current OverlayCmdBuffer so Tab switching during
     * playback still shows that bot's overlays for the scrubbed
     * tick (not whatever they emitted most recently live). */
    for (BYTE oi = 0; oi < MAX_TANKS; oi++) {
        OverlayCmdBuffer *ovl = botManagerGetOverlayCmds(oi);
        if (ovl && ovl->count > 0) {
            f->botOverlayCmdCount[oi] = ovl->count;
            f->botOverlayCmds[oi] = (OverlayCmd *)malloc(
                ovl->count * sizeof(OverlayCmd));
            memcpy(f->botOverlayCmds[oi], ovl->cmds,
                   ovl->count * sizeof(OverlayCmd));
        }
    }

    /* ── A* / Dijkstra path (the key-4 green polyline) ── trace
     * from the followed bot's NORMAL Dijkstra slate to its current
     * goal. Mirrors updateCachedPath()'s logic, but writing into
     * the frame's pathX/pathY/pathLen instead of the live cache. */
    if (pf && f->goalInfoValid
        && f->goalInfo.kind[0] != '\0'
        && strcmp(f->goalInfo.kind, "none") != 0) {
        int slate = brainPathfinderDijkstraFindBest(pf, /* KIND_NORMAL */ 0);
        if (slate >= 0) {
            int tx[2048], ty[2048];
            int n = brainPathfinderDijkstraTracePath(pf, slate,
                f->goalInfo.mx, f->goalInfo.my, tx, ty, 2048);
            if (n > 0) {
                f->pathLen = n;
                f->pathX = (int *)malloc(n * sizeof(int));
                f->pathY = (int *)malloc(n * sizeof(int));
                memcpy(f->pathX, tx, n * sizeof(int));
                memcpy(f->pathY, ty, n * sizeof(int));
            }
        }
    }

    /* ── Per-panel JSON capture ── one poll per registered panel
     * for the followed bot. Pays the brain-eval cost only for
     * panels the user actually has registered, not visible — so
     * no need for "is the window open" gating. */
    int totalN = panelRegistryCount();
    for (int pi = 0; pi < totalN && pi < PANEL_REG_MAX; pi++) {
        const PanelRegistryEntry *e = panelRegistryGet(pi);
        if (!e || e->bot_owner != app->followBot) continue;
        if (!e->lua_expr[0]) continue;
        f->recordedPanels[pi] =
            botManagerEvalLuaString(app->followBot, e->lua_expr);
    }

    /* ── Shot-sim POI poll snapshots ── eval every POI owned by the
     * followed bot and stash the result so playback can re-show the
     * same buttons. Same one-poll-per-tick cost the live panel pays. */
    int npoi = shotSimPoiCount();
    if (npoi > SHOTSIM_POI_REG_MAX) npoi = SHOTSIM_POI_REG_MAX;
    f->shotSimPoiCount = (uint8_t)npoi;
    for (int pi = 0; pi < npoi; pi++) {
        int wx = 0, wy = 0;
        f->shotSimPois[pi].ok = shotSimPoiPollLive(pi, app, &wx, &wy);
        f->shotSimPois[pi].wx = wx;
        f->shotSimPois[pi].wy = wy;
    }

    /* ── viz_detail registry snapshot ── shallow memcpy of the
     * per-tick entries so the inspector dialog can scrub backwards
     * to any frame and see the brain's clickable map primitives +
     * body text from THAT frame. The brain rebuilds the live
     * registry next tick; this snapshot is the only record after
     * that. Strings are inline (fixed-size arrays inside the
     * struct), so a single malloc + memcpy captures everything. */
    {
        int vdc = vizDetailCount();
        if (vdc > 0) {
            f->vizDetails = (VizDetailEntry *)malloc(vdc * sizeof(VizDetailEntry));
            if (f->vizDetails) {
                for (int i = 0; i < vdc; i++) {
                    const VizDetailEntry *src = vizDetailGet(i);
                    if (src) f->vizDetails[i] = *src;
                }
                f->vizDetailCount = vdc;
            } else {
                f->vizDetailCount = 0;
            }
        } else {
            f->vizDetails = NULL;
            f->vizDetailCount = 0;
        }
    }

    /* pill_contrib snapshot — same scrub-back rationale as
     * vizDetails. Each PillContribEntry is fixed-size (inline
     * tile array), so a single malloc + memcpy captures the lot. */
    {
        int pcc = pillContribCount();
        if (pcc > 0) {
            f->pillContrib = (PillContribEntry *)malloc(pcc * sizeof(PillContribEntry));
            if (f->pillContrib) {
                for (int i = 0; i < pcc; i++) {
                    const PillContribEntry *src = pillContribGet(i);
                    if (src) f->pillContrib[i] = *src;
                }
                f->pillContribCount = pcc;
            } else {
                f->pillContribCount = 0;
            }
        } else {
            f->pillContrib = NULL;
            f->pillContribCount = 0;
        }
    }

    /* Strategic placement heatmap — only poll Lua when [8] is on so
     * recordings don't pay the brain query cost on every tick. */
    if (vizFlag(app->regIdxStratPlace) && botManagerIsBot(app->followBot)) {
        f->stratPlaceText = botManagerEvalLuaString(app->followBot,
            "return brain.get_strategic_place_heatmap()");
    } else {
        f->stratPlaceText = NULL;
    }

    rb->count++;
}

/* ------------------------------------------------------------------ */
/* Brain overlay rendering — walks each bot's OverlayCmdBuffer and
 * draws the commands the brain emitted via the overlay_* Lua API.
 * Each command is filtered by its viz_idx against the runtime
 * viz registry: if the row is off (or X-key suppress is on and the
 * row isn't hud_resources), the command is skipped.
 *
 * Coordinate system: brain command x/y are in TILE units (1.0 =
 * 256 wu = 16 game pixels). We convert to screen pixels via the
 * same camera math the map renderer uses.
 * ------------------------------------------------------------------ */

static void mapTileToScreen(BrainTestApp *app, float tx, float ty,
                            int screenW, int screenH,
                            float *out_sx, float *out_sy) {
    /* Quantize to game-pixel precision to match the sprite renderer
     * (mapview.c does `q->x >> 4`). Both camera AND per-object
     * positions are floored to whole game pixels, then scaled by
     * zoom — so overlays step in whole game pixels alongside sprites
     * with no sub-pixel drift between them as the camera scrolls.
     *
     * The integer-floored half-viewport `(screenW/(2*zf))*zf` is the
     * other half of the trick: at zoom 3 with screenW=1000 it equals
     * 996, not 1000 — using `screenW/2.0f` here would produce a
     * delta that varies with zoom and makes overlays jump around
     * sprites as zoom changes. */
    int zf       = app->zoomFactor;
    int centerPX = (int)app->viewCenterX >> 4;     /* wu → game-pixel */
    int centerPY = (int)app->viewCenterY >> 4;
    int scx      = (screenW / (2 * zf)) * zf;
    int scy      = (screenH / (2 * zf)) * zf;
    int pixelX   = (int)floorf(tx * 16.0f);
    int pixelY   = (int)floorf(ty * 16.0f);
    *out_sx = (float)scx + (float)(pixelX - centerPX) * (float)zf;
    *out_sy = (float)scy + (float)(pixelY - centerPY) * (float)zf;
}

/* Sub-game-pixel variant — no floor on the per-object pixel coords.
 * Camera + half-viewport are still snapped to whole game pixels (so
 * the camera doesn't jitter as it scrolls), but the overlay's own
 * position keeps its 1/256-tile precision. Use for annotations that
 * mark a precise WU-level position (shell hit dot, etc.) where the
 * 1-game-pixel quantization in the standard path is visible. */
static void mapTileToScreenPrecise(BrainTestApp *app, float tx, float ty,
                                    int screenW, int screenH,
                                    float *out_sx, float *out_sy) {
    int   zf       = app->zoomFactor;
    int   centerPX = (int)app->viewCenterX >> 4;
    int   centerPY = (int)app->viewCenterY >> 4;
    int   scx      = (screenW / (2 * zf)) * zf;
    int   scy      = (screenH / (2 * zf)) * zf;
    float pixelX   = tx * 16.0f;
    float pixelY   = ty * 16.0f;
    *out_sx = (float)scx + (pixelX - (float)centerPX) * (float)zf;
    *out_sy = (float)scy + (pixelY - (float)centerPY) * (float)zf;
}

static void renderBrainOverlay(BrainTestApp *app, int screenW, int screenH) {
    OverlayCmdBuffer *buf = botManagerGetOverlayCmds(app->followBot);
    if (!buf || buf->count == 0) return;

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < buf->count; i++) {
        OverlayCmd *cmd = &buf->cmds[i];

        /* viz_idx filter — skip if the matching row is off, or if
         * the X-key suppress flag is set (hud_resources stays on
         * because the user always wants the resource counters). */
        if (cmd->viz_idx != OVERLAY_VIZ_IDX_NONE) {
            const VizRegistryEntry *e = vizRegistryGet(cmd->viz_idx);
            if (e) {
                if (app->vizSuppressActive
                    && strcmp(e->id, "hud_resources") != 0) continue;
                if (!e->is_on) continue;
            }
        }

        SDL_SetRenderDrawColor(app->renderer, cmd->r, cmd->g, cmd->b, cmd->a);

        switch (cmd->type) {
        case OVERLAY_CMD_LINE: {
            float sx1, sy1, sx2, sy2;
            mapTileToScreen(app, cmd->x1, cmd->y1, screenW, screenH, &sx1, &sy1);
            mapTileToScreen(app, cmd->x2, cmd->y2, screenW, screenH, &sx2, &sy2);
            SDL_RenderLine(app->renderer, sx1, sy1, sx2, sy2);
            break;
        }
        case OVERLAY_CMD_RECT:
        case OVERLAY_CMD_RECT_FILL:
        case OVERLAY_CMD_RECT_FILL_SUBPIXEL: {
            float sx1, sy1, sx2, sy2;
            if (cmd->type == OVERLAY_CMD_RECT_FILL_SUBPIXEL) {
                mapTileToScreenPrecise(app, cmd->x1, cmd->y1, screenW, screenH, &sx1, &sy1);
                mapTileToScreenPrecise(app, cmd->x2, cmd->y2, screenW, screenH, &sx2, &sy2);
            } else {
                mapTileToScreen(app, cmd->x1, cmd->y1, screenW, screenH, &sx1, &sy1);
                mapTileToScreen(app, cmd->x2, cmd->y2, screenW, screenH, &sx2, &sy2);
            }
            SDL_FRect r = { sx1, sy1, sx2 - sx1, sy2 - sy1 };
            if (cmd->type == OVERLAY_CMD_RECT) {
                SDL_RenderRect(app->renderer, &r);
            } else {
                SDL_RenderFillRect(app->renderer, &r);
            }
            break;
        }
        case OVERLAY_CMD_CIRCLE:
        case OVERLAY_CMD_CIRCLE_SUBPIXEL: {
            float cx, cy;
            if (cmd->type == OVERLAY_CMD_CIRCLE_SUBPIXEL) {
                mapTileToScreenPrecise(app, cmd->x1, cmd->y1, screenW, screenH, &cx, &cy);
            } else {
                mapTileToScreen(app, cmd->x1, cmd->y1, screenW, screenH, &cx, &cy);
            }
            float sr = cmd->radius * 16.0f * app->zoomFactor;
            int seg = (int)(sr * 2.0f);
            if (seg < 12) seg = 12;
            for (int s = 0; s < seg; s++) {
                float a0 = (float)s / seg * 2.0f * 3.14159265f;
                float a1 = (float)(s + 1) / seg * 2.0f * 3.14159265f;
                SDL_RenderLine(app->renderer,
                    cx + cosf(a0) * sr, cy + sinf(a0) * sr,
                    cx + cosf(a1) * sr, cy + sinf(a1) * sr);
            }
            break;
        }
        case OVERLAY_CMD_TEXT: {
            float sx, sy;
            mapTileToScreen(app, cmd->x1, cmd->y1, screenW, screenH, &sx, &sy);
            float scale = (cmd->radius > 0 ? cmd->radius : 1.2f)
                          * ((float)app->zoomFactor / 2.0f);
            int tlen = (int)strlen(cmd->text);
            float tw = tlen * 8.0f * scale;
            float th = 8.0f * scale;
            switch (cmd->anchor) {
            case OVERLAY_ANCHOR_TOPRIGHT:    sx -= tw; break;
            case OVERLAY_ANCHOR_BOTTOMLEFT:  sy -= th; break;
            case OVERLAY_ANCHOR_BOTTOMRIGHT: sx -= tw; sy -= th; break;
            case OVERLAY_ANCHOR_CENTER:      sx -= tw * 0.5f; sy -= th * 0.5f; break;
            }
            SDL_SetRenderScale(app->renderer, scale, scale);
            SDL_RenderDebugText(app->renderer, sx / scale, sy / scale, cmd->text);
            SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
            break;
        }
        case OVERLAY_CMD_HUD_TEXT: {
            /* HUD text — coordinates are pixel offsets from the
             * chosen corner. */
            float sx = cmd->x1, sy = cmd->y1;
            int tlen = (int)strlen(cmd->text);
            float scale = 1.5f;
            float tw = tlen * 8.0f * scale;
            float th = 8.0f * scale;
            switch (cmd->anchor) {
            case OVERLAY_ANCHOR_TOPRIGHT:    sx = (float)screenW - sx - tw; break;
            case OVERLAY_ANCHOR_BOTTOMLEFT:  sy = (float)screenH - sy - th; break;
            case OVERLAY_ANCHOR_BOTTOMRIGHT: sx = (float)screenW - sx - tw;
                                              sy = (float)screenH - sy - th; break;
            }
            SDL_SetRenderScale(app->renderer, scale, scale);
            SDL_RenderDebugText(app->renderer, sx / scale, sy / scale, cmd->text);
            SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
            break;
        }
        case OVERLAY_CMD_CLEAR:
            /* No-op here; the brain calls overlay_clear() at the
             * top of think() to wipe its OWN buffer, not ours. */
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* HUD text rendering                                                  */
/* ------------------------------------------------------------------ */

/* Helper: draw a line of debug text at 2x scale */
static float drawHudLine(SDL_Renderer *r, float x, float y, float scale,
                          uint8_t cr, uint8_t cg, uint8_t cb, const char *text) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_SetRenderScale(r, scale, scale);
    SDL_RenderDebugText(r, x / scale, y / scale, text);
    SDL_SetRenderScale(r, 1.0f, 1.0f);
    return y + 8.0f * scale + 2.0f;
}

static void renderHUD(BrainTestApp *app, int screenW, int screenH) {
    char line[256];

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

    /* Tick counter — show brain tick (sim.tick/2) to match JSONL
     * log, plus the followed bot's most recent think wall time so
     * the user has the headline perf number at a glance without
     * needing the V dialog. The Lua bar below adds replan/phase/goal.
     *
     * In playback mode, show the SCRUBBED frame's tick (and the
     * think time the brain spent on that frame) so the HUD reflects
     * what's actually rendered, not where the live sim has advanced
     * to in the background. */
    uint32_t hudTick = app->sim.tick / 2;
    double   thinkMs = botManagerGetLastThinkMs(app->followBot);
    if (app->playbackMode
        && app->playbackFrame >= 0
        && app->playbackFrame < app->recording.count) {
        const RecordingFrame *pf_ = &app->recording.frames[app->playbackFrame];
        hudTick = pf_->tick / 2;
        thinkMs = (double)pf_->thinkMs;
    }
    SDL_snprintf(line, sizeof(line), "Tick: %u  think: %.2fms  Bot: %d",
                 hudTick, thinkMs, app->followBot);
    float tickScale = 2.0f;
    float tickW = (float)strlen(line) * 8.0f * tickScale;
    float tickH = 8.0f * tickScale + 8.0f;
    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 180);
    SDL_FRect tickBg = { 0, 0, tickW + 12.0f, tickH };
    SDL_RenderFillRect(app->renderer, &tickBg);
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
    SDL_SetRenderScale(app->renderer, tickScale, tickScale);
    SDL_RenderDebugText(app->renderer, 4.0f / tickScale, 4.0f / tickScale, line);
    SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);

    /* CPU-use graph (top-right) — last CPU_HIST_BARS think-times
     * for the followed bot. Color codes: green<5ms, yellow<10ms,
     * orange<20ms, red≥20ms. The 20ms line corresponds to the
     * 50Hz tick budget (TICK_INTERVAL_MS); bars hitting it mean
     * the brain is consuming the entire frame.
     *
     * Source switches in playback mode: instead of the live ring
     * buffer (frozen at whatever live tick was reached when the
     * user entered playback) we source from the recording's per-
     * frame thinkMs, ending at the displayed playback frame. The
     * graph thus slides left as the user scrubs back, so the
     * rightmost bar always corresponds to the current displayed
     * tick. */
    {
        float histBuf[CPU_HIST_BARS];
        int   histN = 0;
        if (app->playbackMode
            && app->playbackFrame >= 0
            && app->playbackFrame < app->recording.count) {
            int end_ = app->playbackFrame;          /* inclusive */
            int start = end_ - (CPU_HIST_BARS - 1);
            if (start < 0) start = 0;
            for (int i = start; i <= end_; i++) {
                histBuf[histN++] = app->recording.frames[i].thinkMs;
            }
        } else if (app->cpuHistCount > 0) {
            int tail = (app->cpuHistCount < CPU_HIST_BARS)
                ? 0
                : app->cpuHistHead;
            for (int i = 0; i < app->cpuHistCount; i++) {
                histBuf[histN++] = app->cpuHist[(tail + i) % CPU_HIST_BARS];
            }
        }

        if (histN > 0) {
            float graphH = 40.0f;
            float barW   = 2.0f;
            float maxMs  = 20.0f;
            float graphW = histN * barW;
            float gx = ((float)screenW - graphW) / 2.0f;
            float gy = 4.0f;

            SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 180);
            SDL_FRect gbg = { gx - 2, gy - 2, graphW + 4, graphH + 14 };
            SDL_RenderFillRect(app->renderer, &gbg);

            for (int i = 0; i < histN; i++) {
                float ms = histBuf[i];
                float h = ms / maxMs * graphH;
                if (h > graphH) h = graphH;
                if (h < 1.0f)   h = 1.0f;

                Uint8 r, g, b;
                if      (ms <  5.0f) { r =  40; g = 200; b =  40; }
                else if (ms < 10.0f) { r = 255; g = 230; b =  40; }
                else if (ms < 20.0f) { r = 255; g = 140; b =  30; }
                else                 { r = 255; g =  50; b =  40; }
                SDL_SetRenderDrawColor(app->renderer, r, g, b, 220);
                SDL_FRect bar = { gx + i * barW, gy + graphH - h,
                                  barW - 1, h };
                SDL_RenderFillRect(app->renderer, &bar);
            }

            /* 20ms budget line. */
            float budgetY = gy + graphH - (20.0f / maxMs * graphH);
            SDL_SetRenderDrawColor(app->renderer, 255, 60, 60, 180);
            SDL_RenderLine(app->renderer, gx, budgetY,
                           gx + graphW, budgetY);

            SDL_SetRenderDrawColor(app->renderer, 180, 180, 180, 200);
            SDL_RenderDebugText(app->renderer, gx, gy + graphH + 2, "0");
            SDL_SetRenderDrawColor(app->renderer, 255, 60, 60, 200);
            SDL_RenderDebugText(app->renderer,
                                gx + graphW + 4, budgetY - 4, "20ms");
        }
    }

    /* The top-right goal/cost/candidate panel that used to live
     * here has been removed — the brain emits richer equivalents
     * (hud_goal, hud_replan, hud_goal_candidates, hud_attack_status,
     * etc.) via the overlay_hud_text Lua API, which renderBrainOverlay
     * draws and the V dialog gates per-id. Keeping a duplicate native
     * panel here would mean two-source-of-truth and more code to
     * decouple from any specific brain. */

    /* Click-cost panel (bottom-right). Shows the most-recent map
     * click's tile coords, A* path cost (or estimate fallback),
     * heuristic estimate, and live Dijkstra cost from the followed
     * bot's KIND_NORMAL slate. In playback the Dijkstra value reads
     * "N/A" — slates aren't recorded per frame, so a live lookup
     * would reflect the current sim, not the scrubbed tick.
     * computeClickPath already uses the playback tank position when
     * scrubbing, so A* / est track history correctly. */
    if (app->clickActive) {
        char l1[160], l2[160];
        const char *dijStr;
        char dijBuf[32];
        if (app->playbackMode) {
            dijStr = "N/A (playback)";
        } else {
            /* Read the same slate the "7" overlay shows (cycled via
             * Shift+7) so the panel agrees with the heatmap pixel
             * under the cursor. The slate's g_cost array is
             * initialized to COST_INF for unreached nodes. */
            BrainPathfinder *pfDij = botManagerGetBrainPathfinder(app->followBot);
            const DijkstraSlate *s = pfDij
                ? brainPathfinderDijkstraGetSlate(pfDij, app->dijViewSlate)
                : NULL;
            int in_boat = (app->sim.sim.tanks[app->followBot] != NULL &&
                           tankIsOnBoat(&app->sim.sim.tanks[app->followBot])) ? 1 : 0;
            float dij = 1e30f;
            if (s && s->g_cost) {
                int ni = (in_boat ? 65536 : 0)
                       + app->clickMY * 256 + app->clickMX;
                dij = s->g_cost[ni];
            }
            if (dij >= 1e29f) {
                dijStr = "unreached";
            } else {
                SDL_snprintf(dijBuf, sizeof(dijBuf),
                             "%.0f (slate %d)", (double)dij, app->dijViewSlate);
                dijStr = dijBuf;
            }
        }
        char astarBuf[32];
        const char *astarStr;
        if (app->clickCost >= 1e29f) {
            astarStr = "unreached";
        } else {
            SDL_snprintf(astarBuf, sizeof(astarBuf), "%.0f", (double)app->clickCost);
            astarStr = astarBuf;
        }
        SDL_snprintf(l1, sizeof(l1),
            "Click (%d,%d)  A*=%s  est=%.0f  dij=%s",
            app->clickMX, app->clickMY,
            astarStr, (double)app->clickEstCost, dijStr);
        SDL_snprintf(l2, sizeof(l2),
            "(A* / est use the followed tank's%s position)",
            app->playbackMode ? " playback" : " live");
        float cs = 1.5f;
        float cw1 = (float)strlen(l1) * 8.0f * cs + 8.0f;
        float cw2 = (float)strlen(l2) * 8.0f * cs + 8.0f;
        float cw  = cw1 > cw2 ? cw1 : cw2;
        float ch  = (8.0f * cs + 2.0f) * 2 + 6.0f;
        float cx  = (float)screenW - cw - 4.0f;
        /* +80 px up so the playback slider doesn't cover the panel. */
        float cy  = (float)screenH - ch - 4.0f - CONTROL_BAR_HEIGHT - 80.0f;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 160);
        SDL_FRect cbg = { cx, cy, cw, ch };
        SDL_RenderFillRect(app->renderer, &cbg);
        drawHudLine(app->renderer, cx + 4.0f, cy + 3.0f, cs, 255, 100, 255, l1);
        drawHudLine(app->renderer, cx + 4.0f, cy + 3.0f + 8.0f * cs + 2.0f,
                    cs, 180, 180, 100, l2);
    }

    /* ── Hover coords (just above the control bar): tile (sub-tile
     * decimal), game pixel (16 wu = 1 px), and raw wu (1/256 tile).
     * Sits above the control bar so it doesn't overlap the scrubber
     * even at low window heights. */
    if (app->hoverValid) {
        float hs = 1.5f;
        float tileX = app->hoverWX / 256.0f;
        float tileY = app->hoverWY / 256.0f;
        float gpxX  = app->hoverWX / 16.0f;
        float gpxY  = app->hoverWY / 16.0f;
        SDL_snprintf(line, sizeof(line),
                     "tile (%.4f,%.4f)  gpx (%.2f,%.2f)  wu (%d,%d)",
                     tileX, tileY, gpxX, gpxY,
                     app->hoverWX, app->hoverWY);
        float hw = (float)strlen(line) * 8.0f * hs + 8.0f;
        float hh = 8.0f * hs + 6.0f;
        float hx = ((float)screenW - hw) / 2.0f;
        float hy = (float)screenH - hh - 4.0f - CONTROL_BAR_HEIGHT;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 140);
        SDL_FRect hbg = { hx, hy, hw, hh };
        SDL_RenderFillRect(app->renderer, &hbg);
        drawHudLine(app->renderer, hx + 4.0f, hy + 3.0f, hs, 200, 200, 200, line);
    }

    /* Overlay legend at bottom-left (toggle with H) */
    if (app->showHUD && (vizFlag(app->regIdxInfluence) || vizFlag(app->regIdxDanger) || vizFlag(app->regIdxFrontLine)
        || vizFlag(app->regIdxPath) || app->clickActive)) {
        float legScale = 2.0f;
        float legH = 8.0f * legScale + 8.0f;
        float legY = (float)screenH - legH;

        char legend[256] = "";
        if (vizFlag(app->regIdxInfluence)) strcat(legend, "Blue=friendly Red=hostile  ");
        if (vizFlag(app->regIdxDanger))    strcat(legend, "Orange=danger  ");
        if (vizFlag(app->regIdxFrontLine)) strcat(legend, "Yellow=frontline  ");
        if (vizFlag(app->regIdxPath))      strcat(legend, "Green=path  ");
        if (app->clickActive)   strcat(legend, "Magenta=click  ");

        float legW = (float)strlen(legend) * 8.0f * legScale + 12.0f;
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 160);
        SDL_FRect legendBg = { 0, legY, legW, legH };
        SDL_RenderFillRect(app->renderer, &legendBg);

        SDL_SetRenderDrawColor(app->renderer, 200, 200, 200, 255);
        SDL_SetRenderScale(app->renderer, legScale, legScale);
        SDL_RenderDebugText(app->renderer, 4.0f / legScale, (legY + 4.0f) / legScale, legend);
        SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
    }
}

/* ------------------------------------------------------------------ */
/* Tick and render                                                     */
/* ------------------------------------------------------------------ */

/* Tick is split into two phases (ported from optimize-perf branch):
 *   appTickBrain — pushes viz state, runs brain.think (which writes
 *                   the overlay buffer), updates goal-info cache.
 *   appTickSim   — advances the sim two ticks (game + keys), updates
 *                   the camera, syncs debug pathfinder.
 *
 * The main loop runs sim → brain → render so the rendered sprites
 * AND the overlay buffer reflect the same post-sim state. Without the
 * split, render saw the post-sim sprite positions but the overlays
 * were against the previous tick's state — visible as the orange
 * shell-hit dot trailing the actual shell sprite by one tick. */

static void appTickBrain(BrainTestApp *app) {
    if (!app->simValid || app->numBots == 0) return;

    app->sim.sim.isInMenu = isInMenu;

    pushVizStateToBots(app->vizSuppressActive);

    /* Clear the viz_detail registry ONCE before any bot's think runs.
     * The registry is global, so if each bot called overlay_detail_clear
     * itself the last bot to think would wipe earlier bots' entries —
     * recordingCapture would then snapshot 0 entries even though the
     * followed bot emitted plenty. Clearing here gives every bot's
     * Brain.think a fresh registry that all bots accumulate into; the
     * recording capture below catches the union after botManagerTick. */
    vizDetailRegistryClear();
    /* Same multi-bot rationale: clear pillcontrib once host-side
     * so per-bot Lua emit just appends to the union. */
    pillContribClear();

    botManagerTick(&app->sim, optAI);
    {
        double ms = botManagerGetLastThinkMs(app->followBot);
        app->cpuHist[app->cpuHistHead] = (float)ms;
        app->cpuHistHead = (app->cpuHistHead + 1) % CPU_HIST_BARS;
        if (app->cpuHistCount < CPU_HIST_BARS) app->cpuHistCount++;
    }

    /* Update goal-info cache for the scrubber's goal-change ticks
     * and per-frame replan markers. */
    app->goalInfoValid =
        botManagerGetGoalInfo(app->followBot, &app->goalInfo);
}

static void appTickSim(BrainTestApp *app) {
    if (!app->simValid || app->numBots == 0) return;

    serverSimTick(&app->sim);
    {
        GameEvent savedEvents[MAX_SNAPSHOT_EVENTS];
        uint8_t savedCount = app->sim.eventCount;
        if (savedCount > 0) {
            memcpy(savedEvents, app->sim.events,
                   savedCount * sizeof(GameEvent));
        }
        serverSimTick(&app->sim);
        if (savedCount > 0 && savedCount + app->sim.eventCount <= MAX_SNAPSHOT_EVENTS) {
            memmove(app->sim.events + savedCount, app->sim.events,
                    app->sim.eventCount * sizeof(GameEvent));
            memcpy(app->sim.events, savedEvents,
                   savedCount * sizeof(GameEvent));
            app->sim.eventCount += savedCount;
        }
    }

    if (!app->freeCamera && app->followBot < MAX_TANKS) {
        WORLD wx, wy;
        if (serverSimGetTankState(&app->sim, app->followBot, &wx, &wy)) {
            app->viewCenterX = app->viewCenterX + ((int)wx - (int)app->viewCenterX) / 4;
            app->viewCenterY = app->viewCenterY + ((int)wy - (int)app->viewCenterY) / 4;
        }
    }

    syncDebugPathfinder(app);
}

/* Convenience for callers that want a full tick in one shot
 * (frame-step '.' past end-of-recording). Order matters: sim →
 * brain → capture so the captured frame holds the sim state at
 * tick N AND the overlay buffer the brain just produced from
 * that same state. Capturing between sim and brain would store
 * the previous tick's overlays and replay them one frame stale. */
static void appTick(BrainTestApp *app) {
    appTickSim(app);
    appTickBrain(app);
    recordingCapture(app);
}

static void refreshGoalInfo(BrainTestApp *app) {
    app->goalInfoValid = botManagerGetGoalInfo(app->followBot, &app->goalInfo);

    BrainPathfinder *pf = botManagerGetBrainPathfinder(app->followBot);
    if (pf && app->goalInfoValid && app->goalInfo.mx > 0) {
        WORLD twx, twy;
        bool hasTank = serverSimGetTankState(&app->sim, app->followBot, &twx, &twy);
        /* Always compute the estimate (this is what the brain uses for ranking) */
        if (hasTank) {
            app->targetEstCost = brainPathfinderEstimateCost(pf,
                twx >> 8, twy >> 8,
                app->goalInfo.mx, app->goalInfo.my,
                pf->in_boat);
        }
        if (pf->status == 1 &&
            pf->dest_x == app->goalInfo.mx && pf->dest_y == app->goalInfo.my) {
            app->targetCost = pf->g_cost[app->goalInfo.my * 256 + app->goalInfo.mx];
        } else if (hasTank) {
            app->targetCost = app->targetEstCost;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Control bar (timeline scrubber strip at bottom of window)            */
/* ------------------------------------------------------------------ */

/* Geometry for every clickable element in the control bar. Computed
 * once so the renderer and the mouse handler agree on hit boxes
 * exactly. Layout:
 *
 *   [<] [Nms] [>]   <track ─────────────────>   [>] [>>] [X]   N/M
 *   ^^^         ^                            ^                  ^
 *   speed      speed                       transport       frame
 *   slower     faster                                      counter
 *
 * x ranges are inclusive on the left, exclusive on the right. */
typedef struct {
    int barY;
    float speedSlowerL, speedSlowerR;     /* [<] */
    float speedLabelL,  speedLabelR;
    float speedFasterL, speedFasterR;     /* [>] */
    float trackL, trackR, trackY;         /* scrubber */
    float thumbW;
    float playL, playR;                   /* [>] play replay */
    float jumpLiveL, jumpLiveR;           /* [>>] jump to live */
    float dropFwdL, dropFwdR;             /* [X] drop forward */
    float frameCounterX;
} ControlBarLayout;

static void controlBarLayout(BrainTestApp *app, int screenW, int screenH,
                              ControlBarLayout *out) {
    out->barY = screenH - CONTROL_BAR_HEIGHT;

    float sx = 8.0f;
    out->speedSlowerL = sx;            sx += 28.0f; out->speedSlowerR = sx;
    /* Speed label width depends on current value. */
    char speedStr[32];
    SDL_snprintf(speedStr, sizeof(speedStr),
                 "%dms", SPEED_PRESETS[app->speedIndex]);
    float labelW = (float)strlen(speedStr) * 8.0f;
    out->speedLabelL  = sx;            sx += labelW + 8.0f; out->speedLabelR = sx;
    out->speedFasterL = sx;            sx += 28.0f; out->speedFasterR = sx;
    sx += 8.0f; /* gap before scrubber */

    /* Frame counter text width (right-aligned). */
    char frameStr[32];
    int displayFrame = app->playbackMode ? app->playbackFrame + 1
                                          : app->recording.count;
    SDL_snprintf(frameStr, sizeof(frameStr), "%d / %d",
                 displayFrame, app->recording.count);
    float frameW = (float)strlen(frameStr) * 8.0f;

    float rightX = (float)screenW;
    out->frameCounterX = rightX - frameW - 8.0f;
    rightX = out->frameCounterX - 4.0f;
    out->dropFwdR  = rightX;     rightX -= 28.0f; out->dropFwdL  = rightX;
    rightX -= 4.0f;
    out->jumpLiveR = rightX;     rightX -= 36.0f; out->jumpLiveL = rightX;
    rightX -= 4.0f;
    out->playR     = rightX;     rightX -= 28.0f; out->playL     = rightX;
    rightX -= 8.0f;

    out->trackL = sx;
    out->trackR = rightX;
    out->trackY = out->barY + CONTROL_BAR_HEIGHT * 0.5f;
    out->thumbW = 8.0f;
}

/* Back-compat shim for the existing scrubberGeometry callers. */
static void scrubberGeometry(BrainTestApp *app, int screenW, int screenH,
                              float *trackL, float *trackR, float *trackY,
                              float *thumbW) {
    ControlBarLayout L;
    controlBarLayout(app, screenW, screenH, &L);
    *trackL = L.trackL; *trackR = L.trackR;
    *trackY = L.trackY; *thumbW = L.thumbW;
}

static void renderControlBar(BrainTestApp *app, int screenW, int screenH) {
    ControlBarLayout L;
    controlBarLayout(app, screenW, screenH, &L);

    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, 20, 20, 30, 220);
    SDL_FRect bg = { 0, (float)L.barY, (float)screenW, CONTROL_BAR_HEIGHT };
    SDL_RenderFillRect(app->renderer, &bg);

    float textY = (float)L.barY + 12.0f;

    /* ── Speed control ── */
    SDL_SetRenderDrawColor(app->renderer, 180, 180, 200, 255);
    SDL_RenderDebugText(app->renderer, L.speedSlowerL + 6, textY, "[<]");
    char speedStr[32];
    SDL_snprintf(speedStr, sizeof(speedStr),
                 "%dms", SPEED_PRESETS[app->speedIndex]);
    SDL_SetRenderDrawColor(app->renderer, 255, 255, 100, 255);
    /* Center the label inside its allocated slot (slot has 8px of
     * extra padding so the text gets 4px on each side). */
    float labelTextX = L.speedLabelL
        + ((L.speedLabelR - L.speedLabelL) - (float)strlen(speedStr) * 8.0f)
          * 0.5f;
    SDL_RenderDebugText(app->renderer, labelTextX, textY, speedStr);
    SDL_SetRenderDrawColor(app->renderer, 180, 180, 200, 255);
    SDL_RenderDebugText(app->renderer, L.speedFasterL + 6, textY, "[>]");

    if (app->recording.count <= 0) {
        SDL_SetRenderDrawColor(app->renderer, 130, 130, 150, 255);
        SDL_RenderDebugText(app->renderer, L.trackL, textY,
                            "(no frames recorded yet)");
        return;
    }

    float trackW = L.trackR - L.trackL;
    if (trackW < 20.0f) return;

    /* Track background line. */
    SDL_SetRenderDrawColor(app->renderer, 60, 60, 80, 255);
    SDL_RenderLine(app->renderer, L.trackL, L.trackY, L.trackR, L.trackY);

    /* Filled portion up to current position. */
    float fillFrac = app->playbackMode
        ? (float)app->playbackFrame / (float)(app->recording.count - 1)
        : 1.0f;
    if (fillFrac < 0) fillFrac = 0;
    if (fillFrac > 1) fillFrac = 1;
    float fillX = L.trackL + fillFrac * trackW;
    SDL_SetRenderDrawColor(app->renderer, 80, 140, 220, 255);
    SDL_RenderLine(app->renderer, L.trackL, L.trackY, fillX, L.trackY);

    /* Goal-change tick markers (yellow vertical lines). Detected by
     * diffing consecutive frames' goalInfo — simpler than the
     * optimize branch which had an explicit wasReplan flag. */
    SDL_SetRenderDrawColor(app->renderer, 255, 220, 50, 200);
    for (int i = 1; i < app->recording.count; i++) {
        if (!app->recording.frames[i].goalInfoValid) continue;
        if (!app->recording.frames[i-1].goalInfoValid) continue;
        const BrainGoalInfo *cur  = &app->recording.frames[i].goalInfo;
        const BrainGoalInfo *prev = &app->recording.frames[i-1].goalInfo;
        if (strcmp(cur->kind, prev->kind) != 0
            || cur->target_id != prev->target_id
            || cur->mx != prev->mx
            || cur->my != prev->my) {
            float mx = L.trackL + ((float)i / (float)(app->recording.count - 1)) * trackW;
            SDL_RenderLine(app->renderer, mx, L.trackY - 5.0f, mx, L.trackY + 5.0f);
        }
    }

    /* Thumb. */
    SDL_FRect thumb = { fillX - L.thumbW * 0.5f, L.trackY - 6.0f,
                        L.thumbW, 12.0f };
    SDL_SetRenderDrawColor(app->renderer, 200, 200, 240, 255);
    SDL_RenderFillRect(app->renderer, &thumb);

    /* ── Transport buttons ── colored brighter when actionable. */
    /* [>] play replay — only meaningful in playback mode. */
    SDL_SetRenderDrawColor(app->renderer,
        100, 255, 100, app->playbackMode ? 255 : 90);
    SDL_RenderDebugText(app->renderer, L.playL + 6, textY, "[>]");
    /* [>>] jump to live. */
    SDL_SetRenderDrawColor(app->renderer, 100, 200, 255, 255);
    SDL_RenderDebugText(app->renderer, L.jumpLiveL + 6, textY, "[>>]");
    /* [X] drop forward — only meaningful in playback. */
    SDL_SetRenderDrawColor(app->renderer,
        255, 80, 80, app->playbackMode ? 255 : 90);
    SDL_RenderDebugText(app->renderer, L.dropFwdL + 6, textY, "[X]");

    /* Frame counter on the far right. */
    char frameStr[32];
    int displayFrame = app->playbackMode
        ? app->playbackFrame + 1
        : app->recording.count;
    SDL_snprintf(frameStr, sizeof(frameStr), "%d / %d",
                 displayFrame, app->recording.count);
    SDL_SetRenderDrawColor(app->renderer, 180, 180, 200, 255);
    SDL_RenderDebugText(app->renderer, L.frameCounterX, textY, frameStr);

    /* Mode hint at top-left of the bar when scrubbing. */
    if (app->playbackMode) {
        SDL_SetRenderDrawColor(app->renderer, 255, 200, 80, 255);
        SDL_RenderDebugText(app->renderer,
                            L.trackL, (float)L.barY + 2.0f, "PLAYBACK");
    }
}

/* WU → screen coords. WU is 1/256-tile (256 WU = one tile). Used by
 * the shot-sim overlay to draw origin/target endpoint markers and
 * the connecting flight line at sub-tile precision. */
static void mapToScreenWU(BrainTestApp *app, float wx, float wy,
                           int screenW, int screenH,
                           float *outSX, float *outSY) {
    float tilex = wx / 256.0f;
    float tiley = wy / 256.0f;
    int zf = app->zoomFactor;
    int tilePixels = 16 * zf;
    int centerMX = app->viewCenterX >> 8;
    int centerMY = app->viewCenterY >> 8;
    float scx = screenW / 2.0f;
    float scy = screenH / 2.0f;
    float mapOriginX = scx - centerMX * tilePixels
                       - (float)(app->viewCenterX & 0xFF) * tilePixels / 256.0f;
    float mapOriginY = scy - centerMY * tilePixels
                       - (float)(app->viewCenterY & 0xFF) * tilePixels / 256.0f;
    *outSX = mapOriginX + tilex * tilePixels;
    *outSY = mapOriginY + tiley * tilePixels;
}

/* Draw the shot-simulator overlay on top of the live map.
 * Three independent layers, each drawn only when its data exists:
 *   - per-tile yellow squares + yellow flight line — only when a
 *     simulation result is cached (auto-runs after both endpoints
 *     are set, so this lights up immediately too)
 *   - green square at the origin — as soon as origin is set
 *   - red X at the target — as soon as target is set
 * The endpoint markers are read straight from the panel's current
 * state, so they appear the moment the user clicks / picks an
 * endpoint, even before a Run has produced tile data. */
static void renderShotSimResult(BrainTestApp *app, int screenW, int screenH) {
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);

    if (app->shotHasResult && app->shotTileCount > 0) {
        int tilePx = 16 * app->zoomFactor;
        for (int i = 0; i < app->shotTileCount; i++) {
            float tcx, tcy;
            mapToScreenWU(app,
                          (float)(app->shotTiles[i].mx * 256 + 128),
                          (float)(app->shotTiles[i].my * 256 + 128),
                          screenW, screenH, &tcx, &tcy);
            SDL_FRect r = { tcx - tilePx * 0.5f, tcy - tilePx * 0.5f,
                            (float)tilePx, (float)tilePx };
            SDL_SetRenderDrawColor(app->renderer, 255, 220, 60, 90);
            SDL_RenderFillRect(app->renderer, &r);
            SDL_SetRenderDrawColor(app->renderer, 255, 220, 60, 200);
            SDL_RenderRect(app->renderer, &r);
        }
        float ox, oy, tx, ty;
        mapToScreenWU(app, (float)app->shotOriginWX, (float)app->shotOriginWY,
                      screenW, screenH, &ox, &oy);
        mapToScreenWU(app, (float)app->shotTargetWX, (float)app->shotTargetWY,
                      screenW, screenH, &tx, &ty);
        SDL_SetRenderDrawColor(app->renderer, 255, 240, 100, 255);
        SDL_RenderLine(app->renderer, ox, oy, tx, ty);
    }

    int wx, wy;
    if (shotSimPanelGetOrigin(&wx, &wy)) {
        float sx, sy;
        mapToScreenWU(app, (float)wx, (float)wy, screenW, screenH, &sx, &sy);
        SDL_SetRenderDrawColor(app->renderer, 60, 220, 60, 230);
        SDL_FRect omark = { sx - 5, sy - 5, 10, 10 };
        SDL_RenderFillRect(app->renderer, &omark);
    }
    if (shotSimPanelGetTarget(&wx, &wy)) {
        float sx, sy;
        mapToScreenWU(app, (float)wx, (float)wy, screenW, screenH, &sx, &sy);
        SDL_SetRenderDrawColor(app->renderer, 255, 60, 60, 255);
        SDL_RenderLine(app->renderer, sx - 6, sy - 6, sx + 6, sy + 6);
        SDL_RenderLine(app->renderer, sx - 6, sy + 6, sx + 6, sy - 6);
    }
}

/* Shot-sim run callback — auto-invoked when both endpoints are set.
 * Stores the full simulation result on the app for renderShotSimResult
 * to pick up next frame. When useTankAngle is true the panel is in
 * "Shoot from tank" mode — fire from origin at the live tank float
 * angle (bit-exact with the engine) instead of inferring from the
 * synthetic target. */
static void shotSimRunCallback(int oWX, int oWY, int tWX, int tWY,
                                int shooter, bool useTankAngle,
                                float tankAngle, void *ud) {
    BrainTestApp *app = (BrainTestApp *)ud;
    app->shotOriginWX    = oWX;
    app->shotOriginWY    = oWY;
    app->shotTargetWX    = tWX;
    app->shotTargetWY    = tWY;
    app->shotShooterType = shooter;
    if (useTankAngle) {
        app->shotTileCount = brainPathfinderSimulateShotAngle(
            (WORLD)oWX, (WORLD)oWY, tankAngle,
            shooter, 0, app->shotTiles,
            (int)(sizeof(app->shotTiles) / sizeof(app->shotTiles[0])));
    } else {
        app->shotTileCount = brainPathfinderSimulateShot(
            (WORLD)oWX, (WORLD)oWY, (WORLD)tWX, (WORLD)tWY,
            shooter, 0, app->shotTiles,
            (int)(sizeof(app->shotTiles) / sizeof(app->shotTiles[0])));
    }
    app->shotHasResult = (app->shotTileCount > 0);
}

/* Shot-sim Clear callback — drops the cached tile list so the
 * yellow overlay disappears next frame. The panel resets its own
 * endpoint state inline. */
static void shotSimClearCallback(void *ud) {
    BrainTestApp *app = (BrainTestApp *)ud;
    app->shotHasResult = false;
    app->shotTileCount = 0;
}

/* Shot-sim "From tank" callback — fills WU coords from the followed
 * tank's current position. Returns false if no tank is followed or
 * the tank slot isn't populated. */
/* Find the recorded TankSnapshot for the followed bot at the active
 * playback frame, or NULL if not found / not in playback. */
static const TankSnapshot *playbackTankForFollowed(const BrainTestApp *app) {
    if (!app->playbackMode) return NULL;
    int f = app->playbackFrame;
    if (f < 0 || f >= app->recording.count) return NULL;
    const RecordingFrame *pf = &app->recording.frames[f];
    BYTE pn = app->followBot;
    for (int i = 0; i < pf->tankCount; i++) {
        if (pf->tanks[i].playerNum == pn) return &pf->tanks[i];
    }
    return NULL;
}

static bool shotSimTankPosCallback(int *outWX, int *outWY, void *ud) {
    BrainTestApp *app = (BrainTestApp *)ud;
    if (!app->simValid) return false;
    BYTE pn = app->followBot;
    if (pn >= MAX_TANKS) return false;
    /* In playback, read the snapshot for the displayed frame instead of
     * the live sim — by the time the shot-sim panel renders, the live
     * tank pointers have already been restored (see playback render
     * teardown), so the live values don't match what the user sees. */
    const TankSnapshot *ts = playbackTankForFollowed(app);
    if (ts) {
        *outWX = (int)ts->worldX;
        *outWY = (int)ts->worldY;
        return true;
    }
    struct tankObj *t = app->sim.sim.tanks[pn];
    if (!t) return false;
    *outWX = (int)t->x;
    *outWY = (int)t->y;
    return true;
}

/* Live float angle of the followed tank — used by the shot-sim
 * panel's "Shoot from tank" target mode so the simulation matches
 * the engine's actual shell flight bit-exactly. In playback, returns
 * the angle from the displayed frame's snapshot. */
static bool shotSimTankAngleCallback(float *outAngle, void *ud) {
    BrainTestApp *app = (BrainTestApp *)ud;
    if (!app->simValid) return false;
    BYTE pn = app->followBot;
    if (pn >= MAX_TANKS) return false;
    const TankSnapshot *ts = playbackTankForFollowed(app);
    if (ts) {
        /* Wire format is angle*256 (matches the playback render
         * reconstruction in this file). */
        *outAngle = (float)ts->angle / 256.0f;
        return true;
    }
    struct tankObj *t = app->sim.sim.tanks[pn];
    if (!t) return false;
    *outAngle = (float)t->angle;
    return true;
}

/* Shot-sim POI poll callback — evaluates the registered POI's
 * lua_expr against the followed bot. Expression returns either two
 * integers (wx, wy) or nil. We use a small Lua snippet to coerce
 * both forms into a single string we can parse here. Returns false
 * (greys out the button) when nil / parse fails. */
/* Live POI eval — evaluates the registered POI's lua_expr against
 * the followed bot's current Lua state. Shared by the live panel
 * callback below and recordingCapture (so playback frames carry the
 * exact value the panel would have shown at that tick). */
static bool shotSimPoiPollLive(int idx, BrainTestApp *app, int *outWX, int *outWY) {
    const ShotSimPoiEntry *e = shotSimPoiGet(idx);
    if (!e || e->bot_owner != app->followBot) return false;
    char wrap[SHOTSIM_POI_REG_EXPR_MAX + 256];
    SDL_snprintf(wrap, sizeof(wrap),
        "local _x,_y = (function() %s end)(); "
        "if _x == nil or _y == nil then return nil end "
        "return tostring(math.floor(_x))..','..tostring(math.floor(_y))",
        e->lua_expr);
    char *body = botManagerEvalLuaString(app->followBot, wrap);
    if (!body || !body[0]) { free(body); return false; }
    int wx = 0, wy = 0;
    bool ok = (sscanf(body, "%d,%d", &wx, &wy) == 2);
    free(body);
    if (!ok) return false;
    *outWX = wx;
    *outWY = wy;
    return true;
}

static bool shotSimPoiPollCallback(int idx, int *outWX, int *outWY, void *ud) {
    BrainTestApp *app = (BrainTestApp *)ud;
    /* In playback, return the recorded value for the displayed frame
     * instead of polling the live brain — same reason as the tank
     * pos/angle callbacks. */
    if (app->playbackMode) {
        int f = app->playbackFrame;
        if (f >= 0 && f < app->recording.count) {
            const RecordingFrame *pf = &app->recording.frames[f];
            if (idx >= 0 && idx < (int)pf->shotSimPoiCount
                && pf->shotSimPois[idx].ok) {
                *outWX = pf->shotSimPois[idx].wx;
                *outWY = pf->shotSimPois[idx].wy;
                return true;
            }
        }
        return false;
    }
    return shotSimPoiPollLive(idx, app, outWX, outWY);
}

static void appRender(BrainTestApp *app) {
    int screenW, screenH;
    SDL_GetWindowSize(app->window, &screenW, &screenH);

    /* Refresh goal info every frame (works while paused too) */
    refreshGoalInfo(app);

    /* Reset the shot-sim panel when the brain enters a new pill take
     * (different target_id, or transition into attack_pill from
     * something else). Endpoints picked while attacking pill A would
     * otherwise linger pointing at A's stale standoff/aim coords
     * once the brain moves on to pill B. */
    static char  prevGoalKind[32]      = "";
    static int   prevGoalTargetId      = -1;
    if (app->goalInfoValid) {
        bool isPillTake = strcmp(app->goalInfo.kind, "attack_pill") == 0;
        bool changedTo  = isPillTake
                       && (strcmp(prevGoalKind, "attack_pill") != 0
                           || prevGoalTargetId != app->goalInfo.target_id);
        if (changedTo) {
            shotSimPanelClearAll();
            app->shotHasResult = false;
            app->shotTileCount = 0;
        }
        SDL_strlcpy(prevGoalKind, app->goalInfo.kind, sizeof(prevGoalKind));
        prevGoalTargetId = app->goalInfo.target_id;
    }

    SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
    SDL_RenderClear(app->renderer);

    if (app->simValid) {
        /* ── PLAYBACK PATCH-IN ──
         * In playback mode we patch the recorded snapshot's data
         * into the live sim structures BEFORE rendering, then
         * restore the live values AFTER. This way every existing
         * renderer (mapViewRenderCentered, overlays, brain
         * commands) draws the recorded state with no per-renderer
         * playback awareness needed. */
        /* Save buffers are static — they're identically sized every
         * frame (full map + 3 grids), so reusing them avoids ~448KB
         * of malloc/free per playback render frame (~27 MB/s of
         * allocator traffic at 60fps). Lazily-allocated on first
         * playback render; freed at shutdown via recordingDestroy
         * is NOT enough — these aren't part of the recording — so
         * we leak at exit, which is fine for a debug tool. */
        static BYTE     *savedMapBytes  = NULL;
        static uint16_t *savedDanger    = NULL;
        static int16_t  *savedInflu     = NULL;
        static uint16_t *savedOverl     = NULL;
        const  BYTE     *savedBrainMapPtr = NULL;  /* pointer-swap, not byte-copy */
        BrainPathfinder *pbPf = botManagerGetBrainPathfinder(app->followBot);
        struct basesObj  savedBases;
        struct pillsObj  savedPills;
        /* Temp objects for tanks/lgm/shells. The sim holds POINTERS to
         * these in tanks[] / lgmen[] / shs, so the storage must outlive
         * the render call — keeping them at function scope. Saved
         * pointers below capture what the live sim was pointing at so
         * we can restore after rendering. */
        struct tankObj   tempTanks[MAX_TANKS];
        tank             savedTanks[MAX_TANKS];
        struct lgmObj    tempLgmen[MAX_TANKS];
        lgm              savedLgmen[MAX_TANKS];
        struct shellsObj tempShellNodes[MAX_SNAPSHOT_SHELLS];
        shells           savedShells = NULL;
        /* Explosions: we don't snapshot these per frame, so the
         * cleanest playback behavior is to hide them entirely (live
         * explosions playing back at the wrong tick would be more
         * confusing than missing). Save the head pointers, NULL the
         * lists during render, restore after. */
        explosions       savedExpl = NULL;
        struct tkExplosionObj *savedTkExpl = NULL;
        minesExp         savedMinesExpl = NULL;
        /* Per-bot overlay-cmd buffer pointer-swap. We point each
         * bot's OverlayCmdBuffer at the recorded array for the
         * render then restore. */
        OverlayCmd *savedOvlCmds[MAX_TANKS] = {0};
        int         savedOvlCount[MAX_TANKS] = {0};
        OverlayCmdBuffer *savedOvlBufs[MAX_TANKS] = {0};
        /* A-star / Dijkstra path (key-4 overlay) — patch app->cachedPath. */
        int savedCachedPathLen = 0;
        bool patchedPath = false;
        /* Camera — only restored if we replaced it (free-camera mode
         * keeps the user's pan even during playback). */
        WORLD savedViewCenterX = 0, savedViewCenterY = 0;
        bool patchedCamera = false;
        bool patched = false;
        if (app->playbackMode
            && app->playbackFrame >= 0
            && app->playbackFrame < app->recording.count) {
            recordingReconstructMap(&app->recording, app->playbackFrame);
            RecordingFrame *pf_ = &app->recording.frames[app->playbackFrame];
            int mapSz = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
            int gridSz = 65536;
            /* Lazy first-time alloc of the static save buffers. */
            if (!savedMapBytes) savedMapBytes = (BYTE *)malloc(mapSz);
            if (!savedDanger)   savedDanger   = (uint16_t *)malloc(gridSz * sizeof(uint16_t));
            if (!savedInflu)    savedInflu    = (int16_t  *)malloc(gridSz * sizeof(int16_t));
            if (!savedOverl)    savedOverl    = (uint16_t *)malloc(gridSz * sizeof(uint16_t));
            /* Map terrain. */
            memcpy(savedMapBytes, &app->sim.sim.mp->mapItem[0][0], mapSz);
            memcpy(&app->sim.sim.mp->mapItem[0][0],
                   app->recording.playbackMap, mapSz);
            /* Pathfinder grids + brainMap (used by overlay renderers). */
            if (pbPf) {
                memcpy(savedDanger, pbPf->danger_grid,    gridSz * sizeof(uint16_t));
                memcpy(savedInflu,  pbPf->influence_grid, gridSz * sizeof(int16_t));
                memcpy(savedOverl,  pbPf->overlay_grid,   gridSz * sizeof(uint16_t));
                memcpy(pbPf->danger_grid,    app->recording.playbackDanger,    gridSz * sizeof(uint16_t));
                memcpy(pbPf->influence_grid, app->recording.playbackInfluence, gridSz * sizeof(int16_t));
                memcpy(pbPf->overlay_grid,   app->recording.playbackOverlay,   gridSz * sizeof(uint16_t));
                /* brainMap is externally-owned memory the pathfinder
                 * just holds a pointer to. Swap the pointer to our
                 * playback buffer for the duration of this render
                 * and restore it after — much cheaper than memcpy
                 * and avoids mutating the bot's actual brainMap. */
                if (pbPf->map) {
                    savedBrainMapPtr = pbPf->map;
                    pbPf->map = app->recording.playbackBrainMap;
                }
            }
            /* Bases & pills — full copy, simpler than per-field patch. */
            savedBases = *app->sim.sim.bs;
            savedPills = *app->sim.sim.pb;
            for (int i = 0; i < pf_->baseCount && i < app->sim.sim.bs->numBases; i++) {
                app->sim.sim.bs->item[i].owner  = pf_->snapBases[i].owner;
                app->sim.sim.bs->item[i].armour = pf_->snapBases[i].armour;
                app->sim.sim.bs->item[i].shells = pf_->snapBases[i].shells;
                app->sim.sim.bs->item[i].mines  = pf_->snapBases[i].mines;
            }
            for (int i = 0; i < pf_->pillCount && i < app->sim.sim.pb->numPills; i++) {
                app->sim.sim.pb->item[i].x      = pf_->snapPills[i].x;
                app->sim.sim.pb->item[i].y      = pf_->snapPills[i].y;
                app->sim.sim.pb->item[i].owner  = pf_->snapPills[i].owner;
                app->sim.sim.pb->item[i].armour = pf_->snapPills[i].armour;
                app->sim.sim.pb->item[i].speed  = pf_->snapPills[i].speed;
                app->sim.sim.pb->item[i].inTank = pf_->snapPills[i].inTank ? TRUE : FALSE;
            }

            /* ── Tanks ── reconstruct from TankSnapshot wire entries
             * into temp tankObj structs and re-aim sim.tanks[i] at
             * them. Subset of fields the renderer reads (x/y/angle/
             * speed/onBoat/death/resources). */
            memset(tempTanks, 0, sizeof(tempTanks));
            for (int i = 0; i < MAX_TANKS; i++) {
                savedTanks[i] = app->sim.sim.tanks[i];
                app->sim.sim.tanks[i] = NULL;
            }
            for (int i = 0; i < pf_->tankCount; i++) {
                TankSnapshot *ts = &pf_->tanks[i];
                BYTE pn = ts->playerNum;
                if (pn >= MAX_TANKS) continue;
                struct tankObj *t = &tempTanks[pn];
                t->x         = ts->worldX;
                t->y         = ts->worldY;
                /* TURNTYPE / SPEEDTYPE are floats. Wire format is
                 * actual * 256, so divide by 256.0f to recover
                 * sub-tick precision (integer / 256 would snap
                 * the tank to coarse angle increments and look
                 * janky during scrubbing). */
                t->angle     = (TURNTYPE)((float)ts->angle  / 256.0f);
                t->speed     = (SPEEDTYPE)((float)ts->speed / 256.0f);
                t->onBoat    = (ts->tankStatus & 0x0F) ? TRUE : FALSE;
                t->deathWait = ts->deathWait;
                t->armour    = ts->armour;
                t->shells    = ts->shells;
                t->mines     = ts->mines;
                t->trees     = ts->trees;
                app->sim.sim.tanks[pn] = t;
            }

            /* ── LGMs ── lgmFrame is encoded as actual_frame+1 with 0
             * meaning "in tank or dead" (invisible). Position comes
             * from lgmMX/MY map coords + lgmPX/PY pixel offsets. */
            memset(tempLgmen, 0, sizeof(tempLgmen));
            for (int i = 0; i < MAX_TANKS; i++) {
                savedLgmen[i] = app->sim.sim.lgmen[i];
                app->sim.sim.lgmen[i] = NULL;
            }
            for (int i = 0; i < pf_->tankCount; i++) {
                TankSnapshot *ts = &pf_->tanks[i];
                BYTE pn = ts->playerNum;
                if (pn >= MAX_TANKS) continue;
                if (ts->lgmFrame == 0) continue;  /* in-tank / dead */
                struct lgmObj *l = &tempLgmen[pn];
                l->playerNum = pn;
                l->frame     = ts->lgmFrame - 1;
                l->x = (WORLD)((ts->lgmMX << 8) + (ts->lgmPX << 4));
                l->y = (WORLD)((ts->lgmMY << 8) + (ts->lgmPY << 4));
                l->inTank    = FALSE;
                l->isDead    = FALSE;
                app->sim.sim.lgmen[pn] = l;
            }

            /* ── Shells ── linked list reconstruction. */
            memset(tempShellNodes, 0, sizeof(tempShellNodes));
            savedShells = app->sim.sim.shs;
            app->sim.sim.shs = NULL;
            if (pf_->shellCount > 0) {
                for (int i = 0; i < pf_->shellCount; i++) {
                    struct shellsObj *s = &tempShellNodes[i];
                    s->x         = pf_->snapShells[i].worldX;
                    s->y         = pf_->snapShells[i].worldY;
                    s->angle     = pf_->snapShells[i].angle;
                    s->owner     = pf_->snapShells[i].owner;
                    s->length    = pf_->snapShells[i].length;
                    s->shellDead = FALSE;
                    s->next = (i + 1 < pf_->shellCount) ? &tempShellNodes[i + 1] : NULL;
                    s->prev = (i > 0) ? &tempShellNodes[i - 1] : NULL;
                }
                app->sim.sim.shs = &tempShellNodes[0];
            }

            /* ── Explosions ── hide entirely during playback. Live
             * explosion animation frames don't correspond to anything
             * recorded, so playing them back at a scrubbed tick is
             * just visual noise. Save heads, NULL the lists. */
            savedExpl      = app->sim.sim.expl;
            savedTkExpl    = app->sim.sim.tankExplosions;
            savedMinesExpl = app->sim.sim.minesExplosions;
            app->sim.sim.expl            = NULL;
            app->sim.sim.tankExplosions  = NULL;
            app->sim.sim.minesExplosions = NULL;

            /* ── Brain overlay command buffers ── pointer-swap each
             * active bot's OverlayCmdBuffer to the recorded cmds.
             * We don't memcpy the whole buffer; we just retarget its
             * cmds/count fields and reset them after render. */
            for (BYTE oi = 0; oi < MAX_TANKS; oi++) {
                OverlayCmdBuffer *ovl = botManagerGetOverlayCmds(oi);
                if (!ovl) continue;
                savedOvlBufs[oi]  = ovl;
                savedOvlCmds[oi]  = ovl->cmds;
                savedOvlCount[oi] = ovl->count;
                ovl->cmds  = pf_->botOverlayCmds[oi];
                ovl->count = pf_->botOverlayCmdCount[oi];
            }

            /* ── viz_detail registry ── point reads at the recorded
             * frame's snapshot. Inspector dialog + map click hit-
             * test go through vizDetailGet/HitTest which honor the
             * playback override transparently. Cleared on patch-out
             * so the next live brain tick repopulates the live
             * registry without interference. */
            vizDetailSetPlaybackView(pf_->vizDetails, pf_->vizDetailCount);
            pillContribSetPlaybackView(pf_->pillContrib, pf_->pillContribCount);

            /* ── A* / Dijkstra path overlay ── temporarily replace
             * app->cachedPath_* with the recorded path. The
             * polyline renderer reads these directly, so this is
             * enough. updateCachedPath() (which would re-poll the
             * brain) is gated by playbackMode below. */
            savedCachedPathLen = app->cachedPathLen;
            int plen = pf_->pathLen;
            if (plen > 2048) plen = 2048;
            if (plen > 0 && pf_->pathX && pf_->pathY) {
                /* cachedPath_x/y are fixed-size on the app, so we
                 * memcpy in. Saved length above lets us truncate
                 * back without restoring the bytes — anything
                 * beyond cachedPathLen is treated as junk by the
                 * renderer anyway. */
                memcpy(app->cachedPath_x, pf_->pathX, plen * sizeof(int));
                memcpy(app->cachedPath_y, pf_->pathY, plen * sizeof(int));
                app->cachedPathLen = plen;
            } else {
                app->cachedPathLen = 0;
            }
            patchedPath = true;

            /* ── Camera ── follow the recorded view center unless
             * the user has explicitly enabled free-camera. Lets the
             * scrubber show what was on screen at that tick instead
             * of jumping the camera to the live tank position. */
            if (!app->freeCamera) {
                savedViewCenterX = app->viewCenterX;
                savedViewCenterY = app->viewCenterY;
                app->viewCenterX = pf_->viewCenterX;
                app->viewCenterY = pf_->viewCenterY;
                patchedCamera = true;
            }

            patched = true;
        }

        /* Set perspective to followed bot for correct coloring */
        BYTE prevSelf = app->sim.sim.viewPlayer;
        app->sim.sim.viewPlayer = app->followBot;

        MapViewCtx ctx = { app->renderer, app->tilesTex, app->zoomFactor, 1 };
        mapViewRenderCentered(&ctx, &app->sim.sim,
                              app->viewCenterX, app->viewCenterY,
                              0, 0, screenW, screenH, app->followBot);

        app->sim.sim.viewPlayer = prevSelf;

        /* Debug overlays */
        renderOverlay(app, screenW, screenH);

        /* Numeric cell values (companion to 1/2 grid overlays). */
        renderCellValues(app, screenW, screenH);

        /* Fog-of-war (drawn between BrainTest pathfinder overlays
         * and the brain's own overlays so the brain's stuff isn't
         * obscured). */
        renderFogOverlay(app, screenW, screenH);

        /* Live Dijkstra heatmap from the followed bot's slate. */
        renderDijkstraHeatmap(app, screenW, screenH);

        /* Strategic pill-placement heatmap from Lua (key 8). */
        renderStratPlaceHeatmap(app, screenW, screenH);

        /* cost_to heatmap (filled in by background thread). */
        renderCostToHeatmap(app, screenW, screenH);

        /* Brain-emitted overlay commands (lines/rects/circles/text
         * the brain pushed via the overlay_* Lua API this tick). */
        renderBrainOverlay(app, screenW, screenH);

        /* ── PLAYBACK RESTORE ── unwind every patch we made above
         * so the next sim tick / data poll sees live data. */
        if (patched) {
            int mapSz = MAP_ARRAY_SIZE * MAP_ARRAY_SIZE;
            int gridSz = 65536;
            memcpy(&app->sim.sim.mp->mapItem[0][0], savedMapBytes, mapSz);
            if (pbPf) {
                memcpy(pbPf->danger_grid,    savedDanger, gridSz * sizeof(uint16_t));
                memcpy(pbPf->influence_grid, savedInflu,  gridSz * sizeof(int16_t));
                memcpy(pbPf->overlay_grid,   savedOverl,  gridSz * sizeof(uint16_t));
                if (savedBrainMapPtr) {
                    pbPf->map = savedBrainMapPtr;
                }
            }
            *app->sim.sim.bs = savedBases;
            *app->sim.sim.pb = savedPills;
            for (int i = 0; i < MAX_TANKS; i++) {
                app->sim.sim.tanks[i] = savedTanks[i];
                app->sim.sim.lgmen[i] = savedLgmen[i];
                if (savedOvlBufs[i]) {
                    savedOvlBufs[i]->cmds  = savedOvlCmds[i];
                    savedOvlBufs[i]->count = savedOvlCount[i];
                }
            }
            app->sim.sim.shs            = savedShells;
            app->sim.sim.expl            = savedExpl;
            app->sim.sim.tankExplosions  = savedTkExpl;
            app->sim.sim.minesExplosions = savedMinesExpl;
            if (patchedPath) app->cachedPathLen = savedCachedPathLen;
            if (patchedCamera) {
                app->viewCenterX = savedViewCenterX;
                app->viewCenterY = savedViewCenterY;
            }
            /* NOTE: the viz_detail playback override stays ACTIVE
             * past this point — the D dialog and highlight pass below
             * (mainImGuiBeginFrame / vizDetailWindowRender) need to
             * see the recorded frame's entries, not whatever the live
             * registry happened to hold from the last live tick.
             * Cleared after mainImGuiEndFrame instead. */
        }
    }

    /* HUD */
    renderHUD(app, screenW, screenH);

    /* Timeline scrubber strip at the bottom. */
    renderControlBar(app, screenW, screenH);

    /* Bare-screen indicator when X-key suppress is active. */
    if (app->vizSuppressActive) {
        const char *msg = "VIZ HIDDEN (X to restore)";
        float scale = 1.4f;
        float tw = (float)strlen(msg) * 8.0f * scale;
        float th = 8.0f * scale;
        float x = 8.0f;
        float y = (float)screenH * 0.75f - th * 0.5f;
        SDL_FRect bg = { x - 4.0f, y - 4.0f, tw + 8.0f, th + 8.0f };
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 200);
        SDL_RenderFillRect(app->renderer, &bg);
        SDL_SetRenderDrawColor(app->renderer, 255, 200, 60, 255);
        SDL_RenderRect(app->renderer, &bg);
        SDL_SetRenderScale(app->renderer, scale, scale);
        SDL_SetRenderDrawColor(app->renderer, 255, 220, 80, 255);
        SDL_RenderDebugText(app->renderer, x / scale, y / scale, msg);
        SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);
    }

    /* Shot-sim result on top of the map (under ImGui panels). */
    renderShotSimResult(app, screenW, screenH);

    /* Pill-contribution overlay (shift-2 cycle). Renders one colored
     * rect per tile that the selected pill contributes danger to,
     * with alpha proportional to the contribution value. Honors the
     * playback override transparently via pillContribGet. */
    if (app->pillContribSel > 0) {
        int n = pillContribCount();
        int sel0 = app->pillContribSel - 1; /* shift to 0-based */
        if (sel0 < n) {
            const PillContribEntry *pe = pillContribGet(sel0);
            if (pe) {
                /* Find max value for normalization. */
                float vmax = 1.0f;
                for (int i = 0; i < pe->tile_count; i++) {
                    if (pe->tiles[i].value > vmax) vmax = pe->tiles[i].value;
                }
                SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
                bool show_values = vizFlag(app->regIdxValues);
                for (int i = 0; i < pe->tile_count; i++) {
                    const PillContribTile *t = &pe->tiles[i];
                    float sx, sy, sx2, sy2;
                    mapTileToScreen(app, (float)t->tile_x, (float)t->tile_y,
                                    screenW, screenH, &sx, &sy);
                    mapTileToScreen(app, (float)t->tile_x + 1.0f, (float)t->tile_y + 1.0f,
                                    screenW, screenH, &sx2, &sy2);
                    float bw = sx2 - sx, bh = sy2 - sy;
                    /* Hot orange→red gradient by normalized value. */
                    float norm = t->value / vmax;
                    if (norm > 1.0f) norm = 1.0f;
                    Uint8 a = (Uint8)(60 + norm * 160);
                    Uint8 r = 255;
                    Uint8 g = (Uint8)(180 - norm * 150);
                    Uint8 b = (Uint8)(60  - norm * 50);
                    SDL_SetRenderDrawColor(app->renderer, r, g, b, a);
                    SDL_FRect rr = { sx, sy, bw, bh };
                    SDL_RenderFillRect(app->renderer, &rr);
                    /* Numeric value label centered in the tile when 9 is on
                     * and the tile is big enough on screen to fit text. */
                    if (show_values && bw >= 14.0f) {
                        char buf[16];
                        if (t->value >= 100.0f) {
                            snprintf(buf, sizeof(buf), "%d", (int)(t->value + 0.5f));
                        } else {
                            snprintf(buf, sizeof(buf), "%.1f", t->value);
                        }
                        /* Light text on darker tiles, dark on lighter ones. */
                        if (norm > 0.5f) {
                            SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
                        } else {
                            SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
                        }
                        float tw = (float)strlen(buf) * 8.0f;
                        SDL_RenderDebugText(app->renderer,
                                            sx + (bw - tw) * 0.5f,
                                            sy + (bh - 8.0f) * 0.5f,
                                            buf);
                    }
                }
                /* Pulsing ring on the source pill. */
                float pcx, pcy;
                mapTileToScreen(app, (float)pe->pill_mx + 0.5f, (float)pe->pill_my + 0.5f,
                                screenW, screenH, &pcx, &pcy);
                float pulse = 0.5f + 0.5f * sinf((float)SDL_GetTicks() * 0.005f);
                float pr = (12.0f + pulse * 4.0f);
                SDL_SetRenderDrawColor(app->renderer, 255, 255, 80, 230);
                /* Diamond approximation of a circle outline (cheap). */
                SDL_FPoint pts[5] = {
                    { pcx,      pcy - pr },
                    { pcx + pr, pcy      },
                    { pcx,      pcy + pr },
                    { pcx - pr, pcy      },
                    { pcx,      pcy - pr },
                };
                SDL_RenderLines(app->renderer, pts, 5);
                /* Top-left status: "PILL_CONTRIB k/N  pill_id=X @(mx,my)" */
                char status[128];
                snprintf(status, sizeof(status),
                         "PILL_CONTRIB %d/%d  pill_id=%d @(%d,%d) tiles=%d  [shift-2 cycle]",
                         app->pillContribSel, n,
                         pe->pill_id, pe->pill_mx, pe->pill_my, pe->tile_count);
                SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 200);
                SDL_FRect bg = { 4, 4, (float)(strlen(status) * 8 + 8), 16 };
                SDL_RenderFillRect(app->renderer, &bg);
                SDL_SetRenderDrawColor(app->renderer, 255, 220, 80, 255);
                SDL_RenderDebugText(app->renderer, 8, 8, status);
            }
        }
    }

    /* Viz-detail highlights. Two passes:
     *   - Hovered (cyan, lighter): row-hover preview from the D
     *     dialog. Drawn first so the selected red wins when both
     *     apply to the same id.
     *   - Selected (red, pulsing): row click in D, focused detail
     *     window, or map hit-test.
     * Geometry resolution is identical for both, so we loop. */
    for (int pass = 0; pass < 2; pass++) {
        const char *id = (pass == 0)
            ? vizDetailWindowGetHovered()
            : vizDetailWindowGetSelected();
        if (!id || !id[0]) continue;
        /* Skip the hover pass when it duplicates the selected id —
         * the red highlight would obscure it anyway. */
        if (pass == 0) {
            const char *sel = vizDetailWindowGetSelected();
            if (sel && sel[0] && strncmp(sel, id, VIZDETAIL_ID_MAX) == 0) continue;
        }
        int idx = vizDetailFindByID(id, (int)app->followBot);
        const VizDetailEntry *e = (idx >= 0) ? vizDetailGet(idx) : NULL;
        if (!e) continue;
        /* Resolve geometry to a bounding screen rect. */
        float sx1, sy1, sx2, sy2;
        if (e->kind == VIZDETAIL_KIND_RECT) {
            mapTileToScreen(app, e->x1, e->y1, screenW, screenH, &sx1, &sy1);
            mapTileToScreen(app, e->x2, e->y2, screenW, screenH, &sx2, &sy2);
        } else if (e->kind == VIZDETAIL_KIND_CIRCLE) {
            float cx, cy;
            mapTileToScreen(app, e->x1, e->y1, screenW, screenH, &cx, &cy);
            float sr = e->x2 * 16.0f * app->zoomFactor;
            sx1 = cx - sr; sy1 = cy - sr;
            sx2 = cx + sr; sy2 = cy + sr;
        } else { /* TEXT */
            float cx, cy;
            mapTileToScreen(app, e->x1, e->y1, screenW, screenH, &cx, &cy);
            sx1 = cx - 4; sy1 = cy - 4;
            sx2 = cx + 4; sy2 = cy + 4;
        }
        float bx = sx1 < sx2 ? sx1 : sx2;
        float by = sy1 < sy2 ? sy1 : sy2;
        float bw = fabsf(sx2 - sx1);
        float bh = fabsf(sy2 - sy1);
        /* Always inflate by a 10-px border so the highlight is
         * clearly visible around the primitive (not on top of it).
         * Then bump up to a min size for tiny dots. */
        const float BORDER   = 5.0f;
        const float MIN_SIZE = 32.0f;
        bx -= BORDER; by -= BORDER;
        bw += BORDER * 2; bh += BORDER * 2;
        if (bw < MIN_SIZE) { bx -= (MIN_SIZE - bw) * 0.5f; bw = MIN_SIZE; }
        if (bh < MIN_SIZE) { by -= (MIN_SIZE - bh) * 0.5f; bh = MIN_SIZE; }
        SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
        if (pass == 0) {
            /* Cyan hover preview — flat, no pulse, lower alpha. */
            SDL_SetRenderDrawColor(app->renderer, 80, 220, 255, 60);
            SDL_FRect glow = { bx - 4, by - 4, bw + 8, bh + 8 };
            SDL_RenderFillRect(app->renderer, &glow);
            SDL_SetRenderDrawColor(app->renderer, 80, 220, 255, 110);
            SDL_FRect r = { bx, by, bw, bh };
            SDL_RenderFillRect(app->renderer, &r);
            SDL_SetRenderDrawColor(app->renderer, 200, 245, 255, 220);
            for (int off = 0; off < 2; off++) {
                SDL_FRect rr = { bx - off, by - off,
                                 bw + off * 2, bh + off * 2 };
                SDL_RenderRect(app->renderer, &rr);
            }
        } else {
            /* Pulse alpha 100..220 over a ~1s cycle. */
            float pulse = 0.5f + 0.5f * sinf((float)SDL_GetTicks() * 0.006f);
            int alpha   = (int)(100.0f + pulse * 120.0f);
            SDL_SetRenderDrawColor(app->renderer, 255, 60, 60, alpha / 3);
            SDL_FRect glow = { bx - 6, by - 6, bw + 12, bh + 12 };
            SDL_RenderFillRect(app->renderer, &glow);
            SDL_SetRenderDrawColor(app->renderer, 255, 50, 50, (Uint8)alpha);
            SDL_FRect r = { bx, by, bw, bh };
            SDL_RenderFillRect(app->renderer, &r);
            SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);
            for (int off = 0; off < 4; off++) {
                SDL_FRect rr = { bx - off, by - off,
                                 bw + off * 2, bh + off * 2 };
                SDL_RenderRect(app->renderer, &rr);
            }
        }
    }

    /* Main-window ImGui pass — draws the shot-sim panel (when
     * visible) and any future floating tools. The panel itself
     * polls its POI/tank callbacks to stay in sync with the live
     * brain state. */
    mainImGuiBeginFrame();
    shotSimPanelRender(true,
                       shotSimRunCallback,
                       shotSimClearCallback,
                       shotSimTankPosCallback,
                       shotSimTankAngleCallback,
                       shotSimPoiPollCallback,
                       app,
                       (int)app->followBot);
    mainImGuiRenderShortcuts(&app->showShortcuts);
    vizDetailWindowRender((int)app->followBot);
    mainImGuiEndFrame(app->renderer);
    /* Drop the viz_detail playback override now that the dialog +
     * highlight pass have consumed it. Idempotent — safe to call
     * even if no patch was active this frame. */
    vizDetailClearPlaybackView();
    pillContribClearPlaybackView();

    SDL_RenderPresent(app->renderer);

    /* V dialog (separate window). Drawn last so its own render
     * present doesn't fight with the main window's. */
    if (app->vizWindow && app->vizRenderer
        && !(SDL_GetWindowFlags(app->vizWindow) & SDL_WINDOW_HIDDEN)) {
        int vw, vh;
        SDL_GetWindowSize(app->vizWindow, &vw, &vh);
        vizWindowRender(app->vizRenderer, vw, vh, vizToggleSaveCallback);
    }

    /* Pre-set the panel poll bridge: in playback the callback
     * returns recorded JSON for app->playbackFrame; otherwise it
     * evaluates the brain live. */
    g_panelPollRecording = app->playbackMode ? &app->recording : NULL;
    g_panelPollFrame     = app->playbackMode ? app->playbackFrame : -1;

    /* P dialog (panels). Same pattern as the V window. */
    if (app->panelWindow && app->panelRenderer
        && !(SDL_GetWindowFlags(app->panelWindow) & SDL_WINDOW_HIDDEN)) {
        int pw, ph;
        SDL_GetWindowSize(app->panelWindow, &pw, &ph);
        g_panelPollFollowBot = app->followBot;
        g_panelPollTick      = app->sim.tick / 2; /* brain tick */
        panelWindowRender(app->panelRenderer, pw, ph,
                          (int)app->followBot, panelPollCallback);
    }

    /* Bot-defined custom windows. Each one polls + renders only
     * when its slot is visible, so the cost is bounded by what the
     * user actually opened. */
    g_panelPollFollowBot = app->followBot;
    g_panelPollTick      = app->sim.tick / 2;
    botWindowRenderAll((int)app->followBot, panelPollCallback);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    srand((unsigned int)(time(NULL) ^ getpid()));

    BrainTestApp app;
    memset(&app, 0, sizeof(app));
    app.zoomFactor = 2;
    app.freeCamera = false;
    app.showHUD = false;
    app.speedIndex = DEFAULT_SPEED_INDEX;
    /* Pre-allocate the recording buffers (delta scratchpads etc.) up
     * front; per-frame storage grows on demand inside recordingCapture. */
    recordingInit(&app.recording);

    /* Register the C-side native toggles in the viz registry so
     * they show up alongside brain-registered overlays in the V
     * dialog (one place to find every toggle, one INI to persist
     * them). Cache the indices on the app so number-key handlers
     * and renderers can read/flip in O(1). */
    app.regIdxInfluence = vizRegistryAddNative(
        "Influence",
        "Friendly/hostile territorial influence grid (blue/red)",
        "Reads BrainPathfinder.influence_grid: positive=friendly, "
        "negative=hostile. Stamped per friendly/hostile base + pill.",
        "1", false);
    app.regIdxDanger = vizRegistryAddNative(
        "Danger",
        "Pill danger grid (orange tint, opacity scales with anger)",
        "Reads BrainPathfinder.danger_grid: stamped per hostile/neutral "
        "pill, intensity scales with pill anger.",
        "2", false);
    app.regIdxFrontLine = vizRegistryAddNative(
        "Frontline",
        "Yellow dots where friendly/hostile influence zones meet",
        "Computed from influence_grid sign-flips between adjacent cells.",
        "3", false);
    app.regIdxPath = vizRegistryAddNative(
        "A* / Dijkstra Path",
        "Green polyline of the bot's currently-followed nav path",
        "Traces the active Dijkstra slate (KIND_NORMAL) to the bot's "
        "current goal; falls back to the last A* search.",
        "4", true);
    app.regIdxFog = vizRegistryAddNative(
        "Fog of war",
        "Dark tint on tiles the bot hasn't observed",
        "Reads BrainPathfinder.map for cells still 0xFF / unknown.",
        "6", false);
    app.regIdxValues = vizRegistryAddNative(
        "Cell values",
        "Print the numeric value on each tile of the active 1/2 grid",
        "Companion to Influence / Danger — only renders at zoom >= 1.",
        "9", false);
    app.regIdxDijkstra = vizRegistryAddNative(
        "Dijkstra heatmap",
        "Live cost-to-every-tile from a Dijkstra slate (green->red)",
        "Reads BrainPathfinder.dij_slates[N] g_cost via "
        "brainPathfinderDijkstraCostAt. Shift+7 cycles slate 0..3.",
        "7", false);
    app.dijViewSlate = 0;
    app.regIdxCostTo = vizRegistryAddNative(
        "Cost-to heatmap",
        "Threaded brainPathfinderCostTo sweep from tank (green->red)",
        "Clones the bot's pathfinder + map, runs cost_to in expanding "
        "rings on a worker thread. Shift+5 lowers danger scale to 0.1.",
        "5", false);
    app.regIdxStratPlace = vizRegistryAddNative(
        "Strat placement",
        "Strategic pill-placement scores (green=bad, red=good, yellow=best)",
        "Heatmap from brain.get_strategic_place_heatmap(): best tiles to "
        "drop a pill given the current map control situation.",
        "8", false);
    /* If any of these came back negative the registry is full or hit a
     * duplicate-id collision — vizFlag would silently no-op forever.
     * Surface it loudly at startup so it's obvious during dev. */
    {
        int idxs[9] = { app.regIdxInfluence, app.regIdxDanger,
                        app.regIdxFrontLine, app.regIdxPath,
                        app.regIdxFog, app.regIdxValues,
                        app.regIdxDijkstra, app.regIdxCostTo,
                        app.regIdxStratPlace };
        for (int i = 0; i < 9; i++) {
            if (idxs[i] < 0) {
                SDL_Log("WARN: native viz registration %d failed; "
                        "the corresponding hotkey will be inert", i);
            }
        }
    }

    if (!parseArgs(argc, argv)) return 1;
    app.followBot = (BYTE)optFollow;

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    static const char *aiNames[]   = { "none", "yes", "advantage", "full" };
    static const char *gameNames[] = { "?", "open", "tournament", "strict" };
    fprintf(stderr, "BrainTest - Brain Debug Viewer\n");
    fprintf(stderr, "  Brain:   %s\n", optBrain);
    fprintf(stderr, "  Players: %d\n", optNumPlayers);
    fprintf(stderr, "  Map:     %s\n", optMap[0] ? optMap : "(built-in Everard Island)");
    fprintf(stderr, "  AI:      %s\n", aiNames[optAI]);
    fprintf(stderr, "  Game:    %s\n", gameNames[optGame]);

    /* Initialize SDL */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    initWinboloTimer();
    clientMutexCreate();
    langSetup();

    /* Create window */
    app.window = SDL_CreateWindow("BrainTest",
                                  DEFAULT_WINDOW_W, DEFAULT_WINDOW_H,
                                  SDL_WINDOW_RESIZABLE);
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    app.renderer = SDL_CreateRenderer(app.window, NULL);
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        return 1;
    }
    SDL_SetRenderVSync(app.renderer, 1);

    /* Stand up the main-window ImGui context for floating tool
     * panels (shot sim today, more later). The V/panel/pool
     * sub-windows still each have their own context bound to
     * their own SDL window — those are unaffected. */
    mainImGuiInit(app.window, app.renderer);

    /* Wire the brain → registry callback BEFORE the first bot's
     * Lua state is created (otherwise braintest_viz_register
     * calls during brain.open() are silent no-ops). */
    brainCoreSetVizRegisterCallback(vizRegisterCallback);
    brainCoreSetPanelRegisterCallback(panelRegisterCallback);
    brainCoreSetShotSimPoiRegisterCallback(shotSimPoiRegisterCallback);
    brainCoreSetVizDetailRegisterCallback(vizDetailRegisterCallback);
    brainCoreSetVizDetailAppendBodyCallback(vizDetailAppendBodyCallback);
    brainCoreSetVizDetailClearCallback(vizDetailClearCallback);
    botManagerSetPreThinkHook(preThinkHook);
    brainCoreSetPillContribClearCallback(pillContribClearCallback);
    brainCoreSetPillContribBeginPillCallback(pillContribBeginPillCallback);
    brainCoreSetPillContribAddTileCallback(pillContribAddTileCallback);
    /* Built-in panel renderer: type "text" → plain unformatted
     * dump. Defined in braintest_panelwindow.cpp so it can call
     * ImGui directly. Per-bot panel modules in
     * brains/<bot>/braintest_panels/ self-register their renderers
     * via static initializers; this just makes sure the fallback
     * always exists. */
    panelTypeRegister("text", panelRenderText);

    /* Load manual-control key bindings from the user's WinBolo.ini
     * (POSIX falls back to defaults — no INI parsing wrapper here). */
    loadKeyBindings(&app);

    /* Initialize map view lookup tables */
    mapViewInit();

    /* Build tile atlas */
    SDL_Surface *sheet = tileLoaderBuildSheet(16);
    if (!sheet) {
        fprintf(stderr, "tileLoaderBuildSheet failed\n");
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        return 1;
    }
    app.tilesTex = SDL_CreateTextureFromSurface(app.renderer, sheet);
    SDL_DestroySurface(sheet);
    if (app.tilesTex) {
        SDL_SetTextureScaleMode(app.tilesTex, SDL_SCALEMODE_NEAREST);
    }

    /* Create overlay texture (256x256 RGBA). Force nearest-neighbor
     * scaling — single-pixel markers (frontline yellow dots) get
     * washed out under SDL3's default linear filter when this 256²
     * texture is upscaled to fill the map view. */
    app.overlayTex = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING, 256, 256);
    if (app.overlayTex)
        SDL_SetTextureScaleMode(app.overlayTex, SDL_SCALEMODE_NEAREST);

    /* Load map */
    bool mapLoaded = false;
    if (optMap[0]) {
        mapLoaded = serverSimCreate(&app.sim, optMap, optGame, false, 0, -1);
        if (!mapLoaded) {
            fprintf(stderr, "Failed to load map '%s'\n", optMap);
        }
    }
    if (!mapLoaded) {
        BYTE emap[6000] = E_MAP;
        if (!serverSimCreateCompressed(&app.sim, emap, 5097, optGame, false, 0, -1)) {
            fprintf(stderr, "serverSimCreateCompressed failed\n");
            return 1;
        }
        strncpy(app.sim.mapName, "Everard Island", MAP_STR_SIZE - 1);
    }
    app.sim.lobbyEnabled = false;
    app.sim.state = serverStateRunning;
    app.simValid = true;

    /* Calculate map bounds */
    calcMapBounds(&app);
    app.viewCenterX = ((app.mapMinX + app.mapMaxX) / 2) << 8;
    app.viewCenterY = ((app.mapMinY + app.mapMaxY) / 2) << 8;

    /* Add bots */
    botManagerInit();
    char brainPath[1024];
    /* Set up the panel-recording directory (debug_sessions/<ts>/panels)
     * once at startup. We use a timestamped subdir so multiple BrainTest
     * runs don't trample each other; offline tools / LLMs reading these
     * files just point at the directory printed below. */
    if (g_panelRecordEnabled) {
        time_t t = time(NULL);
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
        SDL_snprintf(g_panelRecordDir, sizeof(g_panelRecordDir),
                     "debug_sessions/%s/panels", ts);
        /* SDL_CreateDirectory is recursive in SDL3. */
        if (!SDL_CreateDirectory(g_panelRecordDir)) {
            SDL_Log("WARN: couldn't create %s — panel recording disabled (%s)",
                    g_panelRecordDir, SDL_GetError());
            g_panelRecordEnabled = false;
            g_panelRecordDir[0] = '\0';
        } else {
            fprintf(stderr, "  Panel recording: %s\n", g_panelRecordDir);
        }
    }

    if (findBrainScript(optBrain, brainPath, sizeof(brainPath))) {
        fprintf(stderr, "  Brain script: %s\n", brainPath);
        /* Extract brain dir basename ("brains/NewAutopilot" or
         * "brains/NewAutopilot/init.lua" -> "NewAutopilot") so the
         * panel-register callback can namespace types with it. */
        char brainName[64] = "";
        {
            const char *src = optBrain;
            const char *last_slash = src;
            for (const char *p = src; *p; p++) {
                if (*p == '/' || *p == '\\') last_slash = p + 1;
            }
            SDL_snprintf(brainName, sizeof(brainName), "%s", last_slash);
            /* Strip ".lua" if the user pointed at a file. */
            size_t bl = strlen(brainName);
            if (bl > 4 && SDL_strcasecmp(brainName + bl - 4, ".lua") == 0) {
                brainName[bl - 4] = '\0';
            }
            /* Drop a trailing slash or '/init' if either snuck in. */
            bl = strlen(brainName);
            if (bl > 0 && (brainName[bl-1] == '/' || brainName[bl-1] == '\\'))
                brainName[bl-1] = '\0';
        }
        for (int i = 0; i < optNumPlayers; i++) {
            char name[32];
            SDL_snprintf(name, sizeof(name), "Bot %d", i);
            /* Tag the panel-register callback for this bot's brain.open(). */
            g_currentInitBot = i;
            SDL_snprintf(g_currentInitBrainName,
                         sizeof(g_currentInitBrainName), "%s", brainName);
            bool ok = botManagerAddBot(&app.sim, (BYTE)i, brainPath, name,
                                       optAI, optGame, false);
            g_currentInitBot = -1;
            g_currentInitBrainName[0] = '\0';
            if (ok) app.numBots++;
        }
        fprintf(stderr, "  Added %d bots\n", app.numBots);
    } else {
        fprintf(stderr, "  WARNING: Brain script not found: %s\n", optBrain);
    }

    /* Create debug pathfinder for click-to-cost queries */
    app.debugPF = brainPathfinderCreate();
    if (!app.debugPF) {
        fprintf(stderr, "WARNING: failed to create debug pathfinder\n");
    }

    fprintf(stderr, "  Map bounds: (%d,%d)-(%d,%d)\n",
            app.mapMinX, app.mapMinY, app.mapMaxX, app.mapMaxY);

    /* Main loop */
    Uint64 lastTickTime = SDL_GetTicks();
    const bool *keystate = SDL_GetKeyboardState(NULL);
    bool autoPauseDone = false;
    bool firstBrainSeeded = false;

    while (!appQuit) {
        /* Process events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Forward to the main window's ImGui context for the
             * floating tool panels (shot sim etc). Internal window-id
             * gate makes this a no-op for events aimed elsewhere. */
            mainImGuiProcessEvent(&ev);
            /* Forward to V dialog's ImGui context (no-op if not init). */
            vizWindowProcessEvent(&ev);
            /* Same for P (panels) dialog. */
            panelWindowProcessEvent(&ev);
            /* Same for every per-bot custom window. */
            botWindowProcessEvent(&ev);
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                appQuit = TRUE;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
                SDL_Window *evWin = SDL_GetWindowFromEvent(&ev);
                if (evWin == app.window) {
                    appQuit = TRUE;
                } else if (evWin == app.vizWindow) {
                    SDL_HideWindow(app.vizWindow);
                } else if (evWin == app.panelWindow) {
                    SDL_HideWindow(app.panelWindow);
                }
                break;
            }

            case SDL_EVENT_KEY_UP: {
                /* Manual control: translate the user's configured
                 * Bolo key bindings (loaded from WinBolo.ini) into
                 * role names and forward releases to the brain. */
                if (!app.manualControl) break;
                const char *role = keyToRole(&app, ev.key.scancode);
                if (role) {
                    char buf[128];
                    SDL_snprintf(buf, sizeof(buf),
                        "if brain and brain.manual_key then "
                        "brain.manual_key('%s',false) end", role);
                    botManagerExecLua(app.followBot, buf);
                }
                break;
            }

            case SDL_EVENT_KEY_DOWN:
                /* Manual control: same as above, for key presses.
                 * We do this BEFORE the repeat check + switch below
                 * so a held key keeps re-asserting the keystate at
                 * SDL's autorepeat rate (close enough to 'held') and
                 * so e.g. the user's configured Shoot key doesn't
                 * trigger a BrainTest hotkey for the same scancode. */
                if (app.manualControl) {
                    const char *role = keyToRole(&app, ev.key.scancode);
                    if (role) {
                        char buf[128];
                        SDL_snprintf(buf, sizeof(buf),
                            "if brain and brain.manual_key then "
                            "brain.manual_key('%s',true) end", role);
                        botManagerExecLua(app.followBot, buf);
                        /* Suppress BrainTest hotkey conflict — the
                         * configured key has been consumed by the
                         * brain. */
                        break;
                    }
                }
                if (ev.key.repeat) break;
                /* Skip BrainTest hotkeys while a text field is being
                 * edited inside the V dialog (its own ImGui context
                 * — has the V-dialog filter input). The main-window
                 * ImGui context has no text inputs today (shot-sim
                 * panel + shortcuts window are all buttons/radios/
                 * tables), so we don't gate on its WantTextInput —
                 * doing so was eating SPACE / TAB / etc. when the
                 * shot-sim panel just had focus without any text
                 * field active. Re-add a main-context gate when a
                 * real text input lands there. */
                if (vizWindowWantsTextInput()) break;
                switch (ev.key.key) {
                case SDLK_SPACE:
                    app.paused = !app.paused;
                    break;
                case SDLK_TAB:
                    /* Cycle to next active bot */
                    for (int tries = 0; tries < MAX_TANKS; tries++) {
                        app.followBot = (app.followBot + 1) % MAX_TANKS;
                        if (botManagerIsBot(app.followBot)) break;
                    }
                    break;
                case SDLK_F:
                    app.freeCamera = !app.freeCamera;
                    break;
                case SDLK_H:
                    app.showHUD = !app.showHUD;
                    break;
                case SDLK_1:
                    vizFlagFlip(app.regIdxInfluence);
                    app.overlayDirty = true;
                    break;
                case SDLK_2:
                    if ((ev.key.mod & SDL_KMOD_SHIFT) && (ev.key.mod & SDL_KMOD_CTRL)) {
                        /* ctrl-shift-2: dedicated off switch (no
                         * cycling through every pill to reach 0). */
                        app.pillContribSel = 0;
                    } else if (ev.key.mod & SDL_KMOD_SHIFT) {
                        /* shift-2: cycle through pill_contrib overlays
                         * one pill at a time. Wraps via 0 (off) so the
                         * user can return to a clean view between cycles. */
                        int n = pillContribCount();
                        app.pillContribSel++;
                        if (app.pillContribSel > n) app.pillContribSel = 0;
                    } else {
                        vizFlagFlip(app.regIdxDanger);
                        app.overlayDirty = true;
                    }
                    break;
                case SDLK_3:
                    vizFlagFlip(app.regIdxFrontLine);
                    app.overlayDirty = true;
                    break;
                case SDLK_4:
                    vizFlagFlip(app.regIdxPath);
                    app.overlayDirty = true;
                    break;
                case SDLK_5: {
                    /* If a worker is still running, treat 5 as a stop.
                     * Otherwise toggle the overlay; on enable, kick off
                     * a fresh sweep from the tank. Shift+5 = lower
                     * danger weighting (matches pill-cost estimator). */
                    if (app.costToThread) {
                        app.costToAbort = true;
                        SDL_WaitThread(app.costToThread, NULL);
                        app.costToThread = NULL;
                        break;
                    }
                    vizFlagFlip(app.regIdxCostTo);
                    if (vizFlag(app.regIdxCostTo)) {
                        bool lowDanger = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
                        startCostToHeatmap(&app, lowDanger);
                    }
                    break;
                }
                case SDLK_6:
                    vizFlagFlip(app.regIdxFog);
                    break;
                case SDLK_7:
                    /* Shift+7 cycles which slate (0..3) the heatmap
                     * reads from; plain 7 toggles the overlay. */
                    if (ev.key.mod & SDL_KMOD_SHIFT) {
                        app.dijViewSlate = (app.dijViewSlate + 1) % 4;
                    } else {
                        vizFlagFlip(app.regIdxDijkstra);
                    }
                    break;
                case SDLK_8:
                    vizFlagFlip(app.regIdxStratPlace);
                    if (!vizFlag(app.regIdxStratPlace)) {
                        SDL_free(app.stratPlaceText);
                        app.stratPlaceText = NULL;
                    }
                    break;
                case SDLK_9:
                    vizFlagFlip(app.regIdxValues);
                    break;
                case SDLK_0: {
                    /* Clear every native row in one shot. */
                    int natives[9] = {
                        app.regIdxInfluence, app.regIdxDanger,
                        app.regIdxFrontLine, app.regIdxPath,
                        app.regIdxFog,       app.regIdxValues,
                        app.regIdxDijkstra,  app.regIdxCostTo,
                        app.regIdxStratPlace,
                    };
                    SDL_free(app.stratPlaceText);
                    app.stratPlaceText = NULL;
                    for (int i = 0; i < 9; i++) {
                        VizRegistryEntry *e = vizRegistryGetMutable(natives[i]);
                        if (e) e->is_on = false;
                    }
                    app.overlayDirty = true;
                    break;
                }
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    if (app.zoomFactor < MAX_ZOOM) app.zoomFactor++;
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    if (app.zoomFactor > MIN_ZOOM) app.zoomFactor--;
                    break;
                case SDLK_V: {
                    /* Show V dialog (lazy-create on first press). */
                    if (vizWindowWantsTextInput()) break;
                    if (!app.vizWindow) {
                        app.vizWindow = SDL_CreateWindow("BrainTest – Visualizations",
                                                         900, 700,
                                                         SDL_WINDOW_RESIZABLE);
                        if (app.vizWindow) {
                            app.vizRenderer = SDL_CreateRenderer(app.vizWindow, NULL);
                            if (app.vizRenderer) {
                                vizWindowInit(app.vizWindow, app.vizRenderer);
                            } else {
                                SDL_DestroyWindow(app.vizWindow);
                                app.vizWindow = NULL;
                            }
                        }
                    } else if (SDL_GetWindowFlags(app.vizWindow) & SDL_WINDOW_HIDDEN) {
                        SDL_ShowWindow(app.vizWindow);
                        SDL_RaiseWindow(app.vizWindow);
                    } else {
                        SDL_HideWindow(app.vizWindow);
                    }
                    break;
                }
                case SDLK_X:
                    /* Bare-screen mode: short-circuit all viz overlays
                     * except hud_resources. Pressing X again restores. */
                    app.vizSuppressActive = !app.vizSuppressActive;
                    break;
                case SDLK_S:
                    /* Toggle the shot-simulator panel. Closing the
                     * panel also clears any armed pick mode (handled
                     * inside shotSimPanelToggle). */
                    shotSimPanelToggle();
                    break;
                case SDLK_D:
                    /* Toggle the viz-detail inspector panel — lists
                     * every clickable map primitive the brain has
                     * registered this tick with read-only multiline
                     * bodies you can copy from. Click a header to
                     * highlight that primitive on the map; click the
                     * primitive on the map to scroll/expand its
                     * entry here. */
                    vizDetailWindowToggle();
                    break;
                case SDLK_M: {
                    app.manualControl = !app.manualControl;
                    /* Notify the brain via the optional hook. Brains
                     * that don't implement Brain.set_manual_mode just
                     * ignore the call. */
                    char buf[96];
                    SDL_snprintf(buf, sizeof(buf),
                        "if brain and brain.set_manual_mode then "
                        "brain.set_manual_mode(%d) end",
                        app.manualControl ? 1 : 0);
                    botManagerExecLua(app.followBot, buf);
                    break;
                }
                case SDLK_F1:
                    app.showShortcuts = !app.showShortcuts;
                    break;
                case SDLK_ESCAPE:
                    appQuit = TRUE;
                    break;
                case SDLK_END:
                    /* Jump back to live (latest tick), exit playback. */
                    app.playbackMode  = false;
                    app.scrubbing     = false;
                    app.playbackFrame = (app.recording.count > 0)
                        ? app.recording.count - 1 : 0;
                    break;
                case SDLK_HOME:
                    /* Jump to start of recording, stay in playback. */
                    if (app.recording.count > 0) {
                        app.playbackMode  = true;
                        app.playbackFrame = 0;
                    }
                    break;
                case SDLK_COMMA:
                    /* Step exactly one frame back. The SDLK_DOWN switch
                     * already early-outs on ev.key.repeat above so a
                     * held key is one press = one frame. Auto-pauses
                     * + enters playback so the scheduler doesn't fight
                     * the manual step.
                     *
                     * Entering playback from live: anchor playbackFrame
                     * at the latest tick first so the FIRST `,` actually
                     * steps back one tick (without this it'd just
                     * trigger playback at frame 0). */
                    if (app.recording.count > 0) {
                        if (!app.playbackMode) {
                            app.playbackFrame = app.recording.count - 1;
                        }
                        app.playbackMode = true;
                        app.paused       = true;
                        app.scrubbing    = false;
                        if (app.playbackFrame > 0) app.playbackFrame--;
                    }
                    break;
                case SDLK_PERIOD:
                    /* Step one frame forward. If we're already at the
                     * latest recorded frame (or the recording is
                     * empty) advance the LIVE sim by one tick instead
                     * — that way `.` keeps working past the end of
                     * history as a single-tick step. Stays paused
                     * after either case. */
                    app.scrubbing = false;
                    if (app.recording.count > 0
                        && app.playbackFrame + 1 < app.recording.count) {
                        app.playbackMode = true;
                        app.paused       = true;
                        app.playbackFrame++;
                    } else {
                        /* Past the end (or empty) → step live. */
                        app.playbackMode = false;
                        app.paused       = true;
                        appTick(&app);
                        app.playbackFrame = app.recording.count > 0
                            ? app.recording.count - 1 : 0;
                    }
                    break;
                default: {
                    /* Last-chance dispatch: bot-defined panel
                     * shortcuts. Built-in keys have already had
                     * their case-arms handled above; whatever
                     * lands here might be a single letter a bot
                     * registered. We only consider plain
                     * single-letter keys (no modifiers) and only
                     * panels owned by the currently-followed bot
                     * — different bot's shortcuts shouldn't fire
                     * when you're watching this one. */
                    SDL_Keycode k = ev.key.key;
                    if (ev.key.mod &
                        (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))
                        break;
                    if (k < 'a' || k > 'z') break;
                    char up = (char)(k - 'a' + 'A');
                    int totalN = panelRegistryCount();
                    for (int pi = 0; pi < totalN; pi++) {
                        const PanelRegistryEntry *e = panelRegistryGet(pi);
                        if (!e || !e->shortcut[0]) continue;
                        if (e->bot_owner != app.followBot) continue;
                        if (e->shortcut[0] == up) {
                            botWindowToggle(pi);
                            break;
                        }
                    }
                    break;
                }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                /* Same windowID gate as the button events — motion
                 * over a sub-window shouldn't update main-window
                 * hover state or pan/scrub the map. */
                if (ev.motion.windowID != SDL_GetWindowID(app.window)) break;
                app.mouseX = ev.motion.x;
                app.mouseY = ev.motion.y;
                {
                    int sw, sh;
                    SDL_GetWindowSize(app.window, &sw, &sh);
                    app.hoverValid = screenToMap(&app, app.mouseX, app.mouseY,
                                                  sw, sh,
                                                  &app.hoverMX, &app.hoverMY);
                    if (app.hoverValid) {
                        screenToWU(&app, app.mouseX, app.mouseY, sw, sh,
                                   &app.hoverWX, &app.hoverWY);
                    }
                }
                /* Drag-to-scrub takes priority over drag-to-pan. */
                if (app.scrubbing) {
                    int sw, sh;
                    SDL_GetWindowSize(app.window, &sw, &sh);
                    float trackL, trackR, trackY, thumbW;
                    scrubberGeometry(&app, sw, sh, &trackL, &trackR,
                                     &trackY, &thumbW);
                    float frac = (ev.motion.x - trackL) / (trackR - trackL);
                    if (frac < 0) frac = 0;
                    if (frac > 1) frac = 1;
                    int f = (int)(frac * (app.recording.count - 1) + 0.5f);
                    app.playbackFrame = f;
                    break;
                }
                /* Drag-to-pan */
                if (app.rightDown) app.rightDragged = true;
                if (app.dragging) {
                    float dx = ev.motion.x - app.dragLastX;
                    float dy = ev.motion.y - app.dragLastY;
                    app.dragLastX = ev.motion.x;
                    app.dragLastY = ev.motion.y;
                    int tilePixels = 16 * app.zoomFactor;
                    /* Convert screen pixel delta to world units */
                    int wmoveX = (int)(dx * 256.0f / (float)tilePixels);
                    int wmoveY = (int)(dy * 256.0f / (float)tilePixels);
                    int cx = (int)app.viewCenterX - wmoveX;
                    int cy = (int)app.viewCenterY - wmoveY;
                    if (cx < 0) cx = 0;
                    if (cy < 0) cy = 0;
                    if (cx > 65535) cx = 65535;
                    if (cy > 65535) cy = 65535;
                    app.viewCenterX = (WORLD)cx;
                    app.viewCenterY = (WORLD)cy;
                    app.freeCamera = true;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                /* Only handle clicks delivered to the MAIN window —
                 * clicks in the panel/V/pool windows shouldn't leak
                 * through and trigger map clicks or scrubber drags. */
                if (ev.button.windowID != SDL_GetWindowID(app.window)) break;
                if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    app.dragging = true;
                    app.dragLastX = ev.button.x;
                    app.dragLastY = ev.button.y;
                    app.freeCamera = true;
                } else if (ev.button.button == SDL_BUTTON_LEFT) {
                    int sw, sh;
                    SDL_GetWindowSize(app.window, &sw, &sh);
                    /* Bottom-strip click → control bar. Pre-empt the
                     * map-click handler so cost-query queries don't
                     * fire while the user is grabbing the timeline. */
                    if (ev.button.y >= sh - CONTROL_BAR_HEIGHT) {
                        ControlBarLayout L;
                        controlBarLayout(&app, sw, sh, &L);
                        float cx = ev.button.x;
                        /* Speed [<] (slower = larger interval). */
                        if (cx >= L.speedSlowerL && cx < L.speedSlowerR) {
                            if (app.speedIndex < NUM_SPEED_PRESETS - 1)
                                app.speedIndex++;
                            break;
                        }
                        /* Speed [>] (faster). */
                        if (cx >= L.speedFasterL && cx < L.speedFasterR) {
                            if (app.speedIndex > 0) app.speedIndex--;
                            break;
                        }
                        if (app.recording.count <= 0) break;
                        /* [>] play replay — exit pause, advance
                         * playbackFrame at chosen speed. Only does
                         * anything in playback mode. */
                        if (cx >= L.playL && cx < L.playR) {
                            if (app.playbackMode) app.paused = false;
                            break;
                        }
                        /* [>>] jump to live. */
                        if (cx >= L.jumpLiveL && cx < L.jumpLiveR) {
                            app.playbackMode  = false;
                            app.scrubbing     = false;
                            app.playbackFrame = app.recording.count - 1;
                            break;
                        }
                        /* [X] drop forward frames + pause at the
                         * scrubbed point in live mode. */
                        if (cx >= L.dropFwdL && cx < L.dropFwdR) {
                            if (app.playbackMode) {
                                recordingTruncate(&app.recording,
                                    app.playbackFrame + 1);
                                app.playbackMode = false;
                                app.paused       = true;
                            }
                            break;
                        }
                        /* Scrubber track. Snap to a goal-change
                         * marker if the click landed within 10 px
                         * of one. */
                        if (cx >= L.trackL - 4 && cx <= L.trackR + 4) {
                            app.scrubbing    = true;
                            app.playbackMode = true;
                            /* Pause replay auto-advance while the
                             * user has the thumb grabbed; otherwise
                             * the tick scheduler fights the drag and
                             * the thumb appears to slip. The user
                             * can press [>] to resume after release. */
                            app.paused       = true;
                            float frac = (cx - L.trackL)
                                       / (L.trackR - L.trackL);
                            if (frac < 0) frac = 0;
                            if (frac > 1) frac = 1;
                            int clickFrame = (int)(frac
                                * (app.recording.count - 1) + 0.5f);
                            int bestSnap = clickFrame;
                            float bestDist = 11.0f;
                            for (int si = 1; si < app.recording.count; si++) {
                                if (!app.recording.frames[si].goalInfoValid) continue;
                                if (!app.recording.frames[si-1].goalInfoValid) continue;
                                const BrainGoalInfo *cur =
                                    &app.recording.frames[si].goalInfo;
                                const BrainGoalInfo *prev =
                                    &app.recording.frames[si-1].goalInfo;
                                if (strcmp(cur->kind, prev->kind) == 0
                                    && cur->target_id == prev->target_id
                                    && cur->mx == prev->mx
                                    && cur->my == prev->my) continue;
                                float mx = L.trackL
                                    + ((float)si / (float)(app.recording.count - 1))
                                      * (L.trackR - L.trackL);
                                float dist = fabsf(cx - mx);
                                if (dist < bestDist) {
                                    bestDist = dist;
                                    bestSnap = si - 1;
                                }
                            }
                            app.playbackFrame = bestSnap;
                        }
                        break;
                    }
                    /* Shot-sim pick mode: if the panel has armed an
                     * endpoint pick, the next left-click on the map
                     * fills it instead of triggering map-click /
                     * brain.on_click. Pill shooter snaps the origin
                     * to the tile center to match in-game pillbox
                     * physics (pillboxes always fire from
                     * MAP_SQUARE_MIDDLE). */
                    if (shotSimPanelGetPick() != SHOTSIM_PICK_NONE) {
                        int cwx = 0, cwy = 0;
                        if (screenToWU(&app, ev.button.x, ev.button.y,
                                        sw, sh, &cwx, &cwy)) {
                            if (shotSimPanelGetPick() == SHOTSIM_PICK_ORIGIN
                                && shotSimPanelGetShooterType()
                                   == BRAIN_SHOT_SHOOTER_PILL) {
                                cwx = ((cwx >> TANK_SHIFT_MAPSIZE)
                                       << TANK_SHIFT_MAPSIZE) | MAP_SQUARE_MIDDLE;
                                cwy = ((cwy >> TANK_SHIFT_MAPSIZE)
                                       << TANK_SHIFT_MAPSIZE) | MAP_SQUARE_MIDDLE;
                            }
                            shotSimPanelSetClickedWU(cwx, cwy);
                        }
                        break;
                    }
                    /* Suppress map clicks that landed on an ImGui
                     * panel — without this, dragging the shot-sim
                     * window across the map would also drop click
                     * paths underneath it. */
                    if (mainImGuiWantsMouse()) break;
                    int cmx, cmy;
                    if (screenToMap(&app, ev.button.x, ev.button.y,
                                     sw, sh, &cmx, &cmy)) {
                        app.clickActive = true;
                        app.clickMX = cmx;
                        app.clickMY = cmy;
                        app.overlayDirty = true;
                        syncDebugPathfinder(&app);
                        computeClickPath(&app, cmx, cmy);

                        /* Viz-detail hit-test: if the click landed on
                         * a registered primitive, scroll/expand the
                         * matching entry in the viz-detail dialog and
                         * make it the highlight target. Use WU-level
                         * precision (sub-tile) so nested rects and
                         * small circles still hit accurately. */
                        {
                            int cwx = 0, cwy = 0;
                            if (screenToWU(&app, ev.button.x, ev.button.y,
                                            sw, sh, &cwx, &cwy)) {
                                float tx = cwx / 256.0f;
                                float ty = cwy / 256.0f;
                                int hit = vizDetailHitTest(tx, ty, (int)app.followBot);
                                if (hit >= 0) {
                                    const VizDetailEntry *e = vizDetailGet(hit);
                                    if (e) vizDetailWindowSelectAndScroll(e->id);
                                }
                            }
                        }

                        /* Forward to brain.on_click(mx, my, {shift,
                         * ctrl, alt}) so brains can implement custom
                         * click handlers — NewAutopilot uses
                         * shift+click to toggle the pill inspector
                         * overlay and ctrl+click to force-attack a
                         * pill. The cost-query path above runs
                         * unconditionally; the brain decides whether
                         * to do anything in addition based on mods. */
                        SDL_Keymod mod = SDL_GetModState();
                        char luaBuf[256];
                        SDL_snprintf(luaBuf, sizeof(luaBuf),
                            "if brain and brain.on_click then "
                            "brain.on_click(%d, %d, {shift=%s, ctrl=%s, alt=%s}) end",
                            cmx, cmy,
                            (mod & SDL_KMOD_SHIFT) ? "true" : "false",
                            (mod & SDL_KMOD_CTRL)  ? "true" : "false",
                            (mod & SDL_KMOD_ALT)   ? "true" : "false");
                        botManagerExecLua(app.followBot, luaBuf);
                    }
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    app.rightDown = true;
                    app.rightDragged = false;
                    app.dragging = true;
                    app.dragLastX = ev.button.x;
                    app.dragLastY = ev.button.y;
                }
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                /* Same gate as BUTTON_DOWN — but always release the
                 * scrubber drag if it was active, regardless of
                 * which window the release happened in (otherwise
                 * dragging out of the main window leaves us stuck
                 * in scrubbing state). */
                if (ev.button.button == SDL_BUTTON_LEFT && app.scrubbing) {
                    app.scrubbing = false;
                    break;
                }
                if (ev.button.windowID != SDL_GetWindowID(app.window)) break;
                if (ev.button.button == SDL_BUTTON_MIDDLE) {
                    app.dragging = false;
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    if (!app.rightDragged) {
                        /* Right-click (no drag) clears the click overlay */
                        app.clickActive = false;
                        app.clickPathLen = 0;
                        app.overlayDirty = true;
                    }
                    app.rightDown = false;
                    app.dragging = false;
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                /* Only zoom on wheel events delivered to the MAIN
                 * window AND not over an ImGui panel inside it —
                 * scrolling the shot-sim panel's collapsing-header
                 * region shouldn't also zoom the underlying map. */
                if (ev.wheel.windowID == SDL_GetWindowID(app.window)
                    && !mainImGuiWantsMouse()) {
                    if (ev.wheel.y > 0) {
                        if (app.zoomFactor < MAX_ZOOM) app.zoomFactor++;
                    } else if (ev.wheel.y < 0) {
                        if (app.zoomFactor > MIN_ZOOM) app.zoomFactor--;
                    }
                }
                break;
            }
        }

        /* Arrow key scrolling (continuous while held) */
        if (keystate[SDL_SCANCODE_LEFT] || keystate[SDL_SCANCODE_RIGHT] ||
            keystate[SDL_SCANCODE_UP] || keystate[SDL_SCANCODE_DOWN]) {
            app.freeCamera = true;
            int scrollAmount = SCROLL_SPEED * 256 / 3;  /* world units per frame (~60fps) */
            int cx = (int)app.viewCenterX;
            int cy = (int)app.viewCenterY;
            if (keystate[SDL_SCANCODE_LEFT])  cx -= scrollAmount;
            if (keystate[SDL_SCANCODE_RIGHT]) cx += scrollAmount;
            if (keystate[SDL_SCANCODE_UP])    cy -= scrollAmount;
            if (keystate[SDL_SCANCODE_DOWN])  cy += scrollAmount;
            /* Clamp to valid WORLD range */
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx > 65535) cx = 65535;
            if (cy > 65535) cy = 65535;
            app.viewCenterX = (WORLD)cx;
            app.viewCenterY = (WORLD)cy;
        }

        /* Tick scheduler. Cadence = SPEED_PRESETS[speedIndex] ms.
         *  - Playback mode: advance app.playbackFrame, no live sim.
         *  - Live mode: per iteration, sim → brain → capture. The
         *    brain runs against the just-advanced sim so overlay
         *    + sprite stay in phase for the live render, AND the
         *    capture stores both together so playback gets the
         *    same matched pair (rather than the previous tick's
         *    overlay buffer as it would if capture ran before
         *    brain). */
        int tickMs = SPEED_PRESETS[app.speedIndex];
        if (!app.paused) {
            Uint64 now = SDL_GetTicks();
            while (now - lastTickTime >= (Uint64)tickMs) {
                if (app.playbackMode) {
                    if (app.playbackFrame + 1 < app.recording.count) {
                        app.playbackFrame++;
                    } else {
                        /* Hit the end of recorded history → snap
                         * back to live and resume the live sim. */
                        app.playbackMode = false;
                    }
                } else {
                    appTickSim(&app);
                    appTickBrain(&app);
                    recordingCapture(&app);
                    firstBrainSeeded = true;
                    if (!autoPauseDone && app.sim.tick >= 4) {
                        app.paused = true;
                        autoPauseDone = true;
                        lastTickTime += tickMs;
                        break;
                    }
                }
                lastTickTime += tickMs;
            }
        } else {
            lastTickTime = SDL_GetTicks();
            /* First-iteration seed: run the brain once against the
             * initial state so the very first paused frame has a
             * populated overlay buffer to render. Without this the
             * splash frame shows sprites with no brain overlays. */
            if (!firstBrainSeeded && app.simValid && app.numBots > 0) {
                appTickBrain(&app);
                firstBrainSeeded = true;
            }
        }

        /* Render */
        appRender(&app);

        /* Cap at ~60fps (vsync should handle this but just in case) */
        SDL_Delay(1);
    }

    /* Cleanup */
    fprintf(stderr, "Shutting down after tick %u\n", app.sim.tick);
    if (app.costToThread) {
        app.costToAbort = true;
        SDL_WaitThread(app.costToThread, NULL);
        app.costToThread = NULL;
    }
    free(app.costToGrid);
    if (app.costToMutex) SDL_DestroyMutex(app.costToMutex);
    recordingDestroy(&app.recording);
    botManagerDestroy(&app.sim);
    if (app.debugPF) brainPathfinderDestroy(app.debugPF);
    if (app.overlayTex) SDL_DestroyTexture(app.overlayTex);
    if (app.tilesTex) SDL_DestroyTexture(app.tilesTex);
    tileLoaderCleanup();
    if (app.simValid) serverSimDestroy(&app.sim);
    /* Persist viz registry state. */
    {
        char path[FILENAME_MAX];
        vizConfigPath(path, sizeof(path));
        vizRegistrySaveIni(path);
    }
    if (app.vizWindow) {
        vizWindowShutdown();
        if (app.vizRenderer) SDL_DestroyRenderer(app.vizRenderer);
        SDL_DestroyWindow(app.vizWindow);
    }
    if (app.panelWindow) {
        panelWindowShutdown();
        if (app.panelRenderer) SDL_DestroyRenderer(app.panelRenderer);
        SDL_DestroyWindow(app.panelWindow);
    }
    botWindowShutdownAll();
    /* Tear down the main-window ImGui context BEFORE destroying the
     * renderer/window it was bound to. */
    mainImGuiShutdown();
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    langCleanup();
    clientMutexDestroy();
    endWinboloTimer();
    SDL_Quit();

    return 0;
}
