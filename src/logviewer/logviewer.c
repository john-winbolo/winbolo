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
 * Name:          logviewer.c
 * Purpose:
 *   Core log viewer loop. Takes ownership of the event loop
 *   until the user exits. Window and renderer are borrowed,
 *   not owned. Follows the same pattern as mapEditorRun().
 *********************************************************/

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include "backend.h"
#include "spectator_drain.h"   /* dep-free seam: drive/drain the spectator feed */
#include "lv_spec_feed.h"      /* the seed and record handling shared with the embed */
#include "spectator_input.h"   /* dep-free seam: read the controller for pan/zoom */
#include "lv_global.h"
#include "clientmutex.h"
#include "lv_event_flash.h"
#include "draw.h"
#include "lv_team_colour.h"
#include "sound.h"
#include "dns.h"
#include "positions.h"
#include "tiles.h"
#include "logviewer.h"
#include "lv_host.h"
#include "lv_stats.h"
#include "blocks.h"       /* lv_blocksGetSource: the open log's own file */
#include "lv_players.h"   /* the selected player, for the single-player map */
#include "lv_spmap.h"
#include "../gui/sdl3/gfx_settings.h"  /* the simplified view setting */

#include <SDL3/SDL.h>

/* ImGui integration headers */
#include "imgui/imgui_context.h"
#include "imgui/imgui_main_menu.h"
#include "imgui/imgui_controls.h"
#include "imgui/imgui_game_info.h"
#include "imgui/imgui_events.h"
#include "imgui/imgui_item_info.h"
#include "imgui/imgui_hud.h"
#include "imgui/imgui_overview.h"
#include "imgui/imgui_tank.h"
#include "imgui/imgui_comments.h"
#include "imgui/imgui_clip_gif.h"
#include "imgui/imgui_dialogs.h"
#include "imgui/imgui_game_viewport.h"
#include "imgui/imgui_scenario_panel.h"
#include "imgui/imgui_dock.h"
#include "lv_dock_fit.h"
#include "lv_fullscreen_key.h"
#include "lv_save_name.h"   /* the file name Save Log File offers */
#include "lv_utf8_path.h"   /* lvFopenUtf8: the dialogs' paths are UTF-8 */
#include "lv_jump_anim.h"   /* the animation of Rewind and Fast Forward */
#include "lv_panel_set.h"   /* the panels a frame of the overview draws */
#include "lv_host_dock.h"   /* the map beside the sidebars, and the first view */
#include "lv_fullscreen_game.h"
#include "lv_window_size.h" /* -size: a window of a fixed size in pixels */
#include "../common/wb_log.h"
#include "imgui/imgui_game_view.h"
#include "imgui/imgui_logviewer_menu.h"
#include "game_view.h"

/* Platform abstraction */
#include "platform/platform_config.h"
#include "platform/platform_dialogs.h"
#include "../gui/sdl3/macos_pinch.h"
#include "../gui/ui_mode.h"
#include "../gui/lang.h"
#ifdef __APPLE__
#include "platform/mac_menubar.h"
#endif

/* Game-view accessors (Phase D of plans/ctrailer.md). game_view.c includes
 * the bolo-side mapview.h headers and so cannot include backend.h (the two
 * share type names but use different layouts). These accessors expose
 * pointers/values from g_lv as void* /scalar so game_view.c can drive the
 * frame assembly without seeing the LogViewerState struct definition. */
void *lv_gameViewGetScreen(void)         { return &g_lv->view;     }
void *lv_gameViewGetMineView(void)       { return &g_lv->mineView; }
void *lv_gameViewGetBases(void)          { return &g_lv->bs;       }
void *lv_gameViewGetPills(void)          { return &g_lv->pb;       }
BYTE  lv_gameViewGetCameraSlot(void)     { return g_lv->cameraSlot; }
bool  lv_gameViewIsHudAlive(BYTE slot)   {
  if (slot >= MAX_TANKS) return false;
  return g_lv->gameViewHud[slot].alive;
}
uint16_t lv_gameViewGetKills(BYTE slot)  {
  if (slot >= MAX_TANKS) return 0;
  return g_lv->kills[slot];
}
uint16_t lv_gameViewGetDeaths(BYTE slot) {
  if (slot >= MAX_TANKS) return 0;
  return g_lv->deaths[slot];
}
uint32_t lv_gameViewGetDeathTimeMs(BYTE slot) {
  if (slot >= MAX_TANKS) return 0;
  return g_lv->gameViewHud[slot].deathTimeMs;
}
/* The caps the tank bars are drawn against, off the viewer's own rules
   holder. The viewer has no sim to ask, and the drawing code reaches its
   state through these accessors rather than the global. */
void lv_gameViewGetTankFulls(BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees) {
  if (shells) *shells = g_lv->rules.tankFullShells;
  if (mines)  *mines  = g_lv->rules.tankFullMines;
  if (armour) *armour = g_lv->rules.tankFullArmour;
  if (trees)  *trees  = g_lv->rules.tankFullTrees;
}
void lv_gameViewGetBaseFulls(BYTE *shells, BYTE *mines, BYTE *armour) {
  if (shells) *shells = g_lv->rules.baseFullShells;
  if (mines)  *mines  = g_lv->rules.baseFullMines;
  if (armour) *armour = g_lv->rules.baseFullArmour;
}
void lv_gameViewGetInventory(BYTE slot, BYTE *shells, BYTE *mines, BYTE *armour, BYTE *trees) {
  if (slot >= MAX_TANKS) {
    if (shells) *shells = 0;
    if (mines)  *mines  = 0;
    if (armour) *armour = 0;
    if (trees)  *trees  = 0;
    return;
  }
  if (shells) *shells = g_lv->tankInv[slot].shells;
  if (mines)  *mines  = g_lv->tankInv[slot].mines;
  if (armour) *armour = g_lv->tankInv[slot].armour;
  if (trees)  *trees  = g_lv->tankInv[slot].trees;
}

/* Pending memory-based log load (set by logViewerRunFromMemory and
   lv_windowOpenMemory) */
static uint8_t *s_pendingZipData = NULL;
static size_t   s_pendingZipLen  = 0;

/* A file to open, recorded by lv_windowOpenFile. Opening happens at the top
   of the next frame rather than in the middle of the menu that asked for it,
   so the load can run over several frames and draw its progress. */
static char s_pendingOpenPath[FILENAME_MAX];
static bool s_pendingOpen = FALSE;

/* A load in progress, and when it began: the progress window waits
   LV_LOAD_PROGRESS_DELAY_MS before it shows, so a log that opens quickly
   never flashes one. Each frame gives the load LV_LOAD_FRAME_MS of work. */
#define LV_LOAD_PROGRESS_DELAY_MS 250
#define LV_LOAD_FRAME_MS          30
static bool   s_loading      = FALSE;
static Uint64 s_loadStartMs  = 0;

/* --------------------------------------------------------------------------
 * Helper: default team colour value for index
 *
 * Palette rows, in the order the colour dialog lists them. Team 1 green and
 * Team 2 red, the game's own ally / enemy reading, since a two-team round
 * lands on exactly those two slots (lv_playersRebuildTeams). Blue and yellow
 * next for the four-team case; the rest keep their old order.
 * -------------------------------------------------------------------------- */
static void getDef(char *dest, int index) {
    /* The one table of the defaults is in lv_team_colour.h. */
    snprintf(dest, 12, "%d", (int)lvTeamColourDefault(index));
}

/* --------------------------------------------------------------------------
 * Sound
 * -------------------------------------------------------------------------- */
void lv_frontEndPlaySound(sndEffects value) {
    if (g_lv->isSoundsPlaying == TRUE) {
        lv_soundPlayEffect(value);
    }
}

void lv_windowDisableSound(void) {
    g_lv->isSoundsPlaying = FALSE;
}

/* --------------------------------------------------------------------------
 * Item info -- forward all data to the ImGui panel
 * -------------------------------------------------------------------------- */
void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y,
                BYTE armour, BYTE shells, BYTE mines, bool inTank) {
    lv_imgui_item_info_update(itemType, itemNumber, owner, x, y, armour, shells, mines, inTank);
}

/* --------------------------------------------------------------------------
 * Game information -- forward to ImGui panel
 * -------------------------------------------------------------------------- */
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor,
                                BYTE versionRevision, char *mapName, BYTE gameType,
                                bool hiddenMines, BYTE aiType, int32_t startDelay,
                                int32_t timeLimit, BYTE *wbnKey, int32_t startTime) {
    lv_imgui_game_info_set(clear, versionMajor, versionMinor, versionRevision,
                        mapName, gameType, hiddenMines, aiType,
                        startDelay, timeLimit, wbnKey, startTime);
}

/* --------------------------------------------------------------------------
 * Events
 * -------------------------------------------------------------------------- */
void lv_windowAddEvent(int eventType, int kind, int mapX, int mapY, char *msg) {
    lv_imgui_events_add(eventType, kind, mapX, mapY, msg);
}

void lv_windowAddHighlight(char *msg, uint32_t seekMs, int mapX, int mapY) {
    lv_imgui_events_add_highlight(msg, seekMs, mapX, mapY);
}

void lv_windowAddSummary(char *msg) {
    lv_imgui_events_add_summary(msg);
}

/* Playback restarts from a snapshot restore and posts its lines again. */
void lv_windowEventsRewound(void) {
    lv_imgui_events_rewound();
}

void lv_windowRemoveEvents(void) {
    lv_imgui_events_clear();
}

/* The events panel holds every event of the file just loaded: the load's
 * play stage posted each line once (lv_screenLoadStep). Called once per file
 * load, after the load-time summary lines are in. The panel is told the list
 * is complete, so it lists the whole log from the first frame and playback
 * only moves a "now" line through it. A live feed has no load and never
 * calls this: its panel goes on collecting line by line. */
void lv_windowCollectAllEvents(void) {
    lv_imgui_events_set_complete();
}

/* --------------------------------------------------------------------------
 * Control state -- ImGui panels read state directly each frame,
 * so these functions are no-ops.
 * -------------------------------------------------------------------------- */
void lv_controlEnable(int id, bool state, unsigned int menuId) {
    (void)id; (void)state; (void)menuId;
}

void lv_controlsEnable(int state) {
    (void)state;
}

/* --------------------------------------------------------------------------
 * Playback control
 * -------------------------------------------------------------------------- */
/* Jump the scrubber to a highlight moment and centre the view on its cell.
 * The seek walks the log and rebuilds the world, the same state lv_windowTimer
 * ticks on the SDL timer thread under lv_clientMutex, so playback is stopped
 * and the lock held across it exactly as the scrubber and keyboard step do.
 * Without that, a click during playback lands lv_shellsAddItem and
 * lv_shellsDestroy on the same list from two threads and corrupts the heap. */
void lv_windowSeekToHighlight(uint32_t ms, int mapX, int mapY) {
    lv_windowSeekToEvent(ms, 1, mapX, mapY, -1);
}

/* The same jump for a marker over the seek bar. The playhead goes to ms,
 * which for an event is the time of its own tick: playback then stands on
 * the tick the event happened in, with the event done.
 *
 * An event at a place on the map (hasCell) puts that place in the middle of
 * the visible map area (lv_screenShowCell; never with "Tank Centred" on or
 * in the game view) and flashes a ring round it, in the colour of the team
 * the event names (team; -1 for none).
 * Where the view cannot move ("Tank Centred", the game view) and does not
 * show the place, the ring is an arrow at the view's edge.
 * Any other event moves only the playhead and leaves the view where the user
 * put it. */
/* What a jump does to the view when the event has a place. */
typedef enum {
    LV_JUMP_VIEW_KEEP = 0, /* the view stays; the ring (or its edge arrow) shows the place */
    LV_JUMP_VIEW_SHOW,     /* the place goes to the middle (lv_screenShowCell) */
    LV_JUMP_VIEW_CENTRE    /* the same, whatever held the view (lv_windowCentreOnEvent) */
} LvJumpView;

/* The jump of lv_windowSeekToEvent, lv_windowSeekToEventKeepView and
 * lv_windowCentreOnEvent. With LV_JUMP_VIEW_CENTRE the place goes to the
 * middle of the map that shows, and "Tank Centred" is not asked about (the
 * caller has turned it off). */
static void lvWindowJumpToEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team,
                                LvJumpView view) {
    unsigned char wasPlaying = g_lv->playIsPlaying;
    if (wasPlaying) {
        lv_windowPause();
    }
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToTimeMs(ms);
    if (hasCell && view == LV_JUMP_VIEW_CENTRE) {
        float startX, startY, lenX, lenY;
        lvHostVisibleMap(&startX, &startY, &lenX, &lenY);
        lv_screenCentreCellIn(mapX, mapY, startX, startY, lenX, lenY, lv_drawGetZoomLevel());
        /* After the seek: the ring is tied to the seek count as it is now. */
        lv_drawEventFlashStart(mapX, mapY, team);
    } else if (hasCell && view == LV_JUMP_VIEW_SHOW) {
        /* The part of the window the map shows in: the window less the menu
         * bar and the sidebars. The place goes to the middle of that, not of
         * the window. */
        LvDockMargins dock = { 0, 0, 0, 0 };
        int winW = 0, winH = 0;
        SDL_GetWindowSize(g_lv->window, &winW, &winH);
        lv_imgui_dock_margins(&dock.left, &dock.top, &dock.right, &dock.bottom);
        lv_screenShowCell(mapX, mapY,
                          (float)(winW - dock.left - dock.right),
                          (float)(winH - (int)lv_imgui_get_menu_bar_height() -
                                  dock.top - dock.bottom),
                          lv_drawGetZoomLevel());
        /* After the seek: the ring is tied to the seek count as it is now. */
        lv_drawEventFlashStart(mapX, mapY, team);
    } else if (hasCell) {
        /* The view stays. The ring is drawn where the place is, or as an
         * arrow at the view's edge when the view does not show it. */
        lv_drawEventFlashStart(mapX, mapY, team);
    } else {
        lv_drawEventFlashCancel();
    }
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    /* Only the game view puts its camera back on the followed tank. In the
     * overview the view is the user's: taking it to the camera slot's tank
     * here moved the map away from the event's place on every jump, so the
     * ring was drawn where the view no longer was. */
    if (lvFlashCameraFollowsTank(g_lv->gameView ? 1 : 0)) {
        lv_imgui_game_view_init_camera(g_lv);
    }
    lv_windowNeedRedraw();
    if (wasPlaying) {
        lv_windowPlay();
    }
    g_lv->wantScreenUpdate = TRUE;
}

void lv_windowSeekToEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team) {
    lvWindowJumpToEvent(ms, hasCell, mapX, mapY, team, LV_JUMP_VIEW_SHOW);
}

/* A plain click on a line of the Events panel: the same jump, and the view
 * stays where the user put it. A line with a place flashes the ring round
 * it, or an arrow at the view's edge when the view does not show it. A
 * click on the selected line is lv_windowCentreOnEvent. */
void lv_windowSeekToEventKeepView(uint32_t ms, int hasCell, int mapX, int mapY, int team) {
    lvWindowJumpToEvent(ms, hasCell, mapX, mapY, team, LV_JUMP_VIEW_KEEP);
}

/* A click on the selected line of the Events panel, or a double click on a
 * line or on a marker over the seek bar: the same jump, and the place goes
 * to the middle of the map that shows (lvHostVisibleMap) whatever held the
 * view. "Tank Centred" is the one thing
 * that holds the overview, so it is turned off, as from the menu, and it
 * stays off: nothing then takes the view back to the tank. The view of one
 * player (the eye of the HUD panel) does not move the map and is left on.
 *
 * A line with no place only seeks, and in the game view the camera is the
 * followed tank's: both are lv_windowSeekToEvent, with nothing turned off. */
void lv_windowCentreOnEvent(uint32_t ms, int hasCell, int mapX, int mapY, int team) {
    if (!hasCell || lvFlashCameraFollowsTank(g_lv->gameView ? 1 : 0)) {
        lv_windowSeekToEvent(ms, hasCell, mapX, mapY, team);
        return;
    }
    if (lv_imgui_get_tank_centred()) {
        lv_imgui_set_tank_centred(0);
    }
    /* A host that turned the flag on by another road. */
    lv_screenTankCentred(0);
    lvWindowJumpToEvent(ms, hasCell, mapX, mapY, team, LV_JUMP_VIEW_CENTRE);
}

/* Fast Forward with no animation: on to the next snapshot, or the end. On a
 * live feed it is on to the head, where the next frame follows the feed
 * again (lv_screenSpecFrameUpdate). */
static void lvWindowFastForwardNow(void) {
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenFastForward();
    lv_drawDirtyScreen();
    /* Defer the render to the flag consumer (embed: lvEmbedFrameTexture;
     * standalone: the main loop) so the repaint pairs with a fresh
     * blit-offset snapshot instead of tearing one frame — see
     * lv_screenPanToTotalPixels. */
    g_lv->wantScreenUpdate = TRUE;
    lv_clientMutexRelease();
}

/* Rewind with no animation: back to the snapshot lv_screenRewind picks, or,
 * at the end of an animation, to the one picked when it started (toKnown). */
static void lvWindowRewindNow(int toKnown, uint32_t toMs) {
    char line[64];
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    if (toKnown) {
        lv_screenRewindToMs(toMs);
    } else {
        lv_screenRewind();
    }
    lv_drawDirtyScreen();
    /* Defer to the flag — see lvWindowFastForwardNow. */
    g_lv->wantScreenUpdate = TRUE;
    lv_screenGetTime(line);
    lv_clientMutexRelease();
    if (g_lv->playIsPlaying == TRUE) {
        lv_controlsEnable(1 /* STATE_PLAYING */);
    } else {
        g_lv->wantScreenUpdate = TRUE;
    }
}

/* --------------------------------------------------------------------------
 * The animation of Rewind and Fast Forward (lv_jump_anim.h)
 *
 * A press does not jump. For LV_JUMP_ANIM_MS the host seeks, one step on
 * each frame that has a new step due, through moments between the time
 * playback is at and the target, and then makes the jump itself: the last
 * move is lv_screenFastForward or lv_screenRewindToMs, so playback ends on
 * the tick, and in the state, a press with no animation gives. Each step is
 * a seek, so the lists of events and messages, the sounds and the pings are
 * as after any seek, and the seek bar, the time and the event markers go
 * with it.
 *
 * While it runs the playback timer plays nothing (lv_windowSetTickHold), so
 * Play and Pause are not touched and are as they were when it ends.
 *
 * The steps are the animation's own seeks: it keeps the seek count
 * (lv_screenSeekSerial) of its last one. A count that has moved on by the
 * next frame is some other seek (the seek bar, a click on an event, a step
 * key, a new log): that seek wins and the animation is dropped where it is.
 * A second press ends the first animation on its target and starts from
 * there, so five quick presses are five jumps.
 * -------------------------------------------------------------------------- */
static struct {
    bool     active;
    bool     back;
    uint32_t fromMs, toMs;
    int      steps;   /* lvJumpAnimSteps */
    int      shown;   /* the last step that was sought to */
    Uint64   startNs;
    uint32_t serial;  /* the seek count after the animation's last seek */
} s_lvJump;

/* TRUE in a host whose loop calls lvHostRenderFrame, which is what moves an
 * animation on. Without it a press jumps. */
static bool s_lvJumpHost = FALSE;

/* Let go of an animation without a move: another seek has the playback. */
static void lvJumpAnimDrop(void) {
    if (!s_lvJump.active) {
        return;
    }
    s_lvJump.active = FALSE;
    lv_clientMutexWaitFor();
    lv_windowSetTickHold(false);
    lv_clientMutexRelease();
}

/* End an animation now, on its target. */
static void lvJumpAnimFinish(void) {
    bool stale;
    if (!s_lvJump.active) {
        return;
    }
    s_lvJump.active = FALSE;
    lv_clientMutexWaitFor();
    lv_windowSetTickHold(false);
    stale = (g_lv->isLoaded == FALSE || lv_screenSeekSerial() != s_lvJump.serial);
    lv_clientMutexRelease();
    if (stale) {
        return;
    }
    if (s_lvJump.back) {
        lvWindowRewindNow(1, s_lvJump.toMs);
    } else {
        lvWindowFastForwardNow();
    }
}

/* Where Fast Forward goes on a live feed: the head, the newest moment the
 * feed has. lv_screenFastForwardTargetMs has no answer there, because the
 * next snapshot is not written yet, but lv_screenFastForward plays on to the
 * head. The caller holds the decoder's mutex. */
static bool lvJumpLiveHeadMs(uint32_t nowMs, uint32_t *toMs) {
    if (!lv_screenSpecIsLiveMode() || g_lv->totalTimeMs <= nowMs) {
        return FALSE;
    }
    *toMs = g_lv->totalTimeMs;
    return TRUE;
}

/* Start one for a press of Rewind (back) or Fast Forward. FALSE when the
 * press is to jump: the option is off, nowhere to go, or a jump with no
 * tick between its ends. On a live feed Fast Forward goes to the head. */
static bool lvJumpAnimStart(bool back) {
    uint32_t nowMs, toMs = 0;
    bool known;
    int steps;

    if (!s_lvJumpHost || !lv_imgui_get_jump_anim() || g_lv->isLoaded == FALSE) {
        return FALSE;
    }
    lv_clientMutexWaitFor();
    nowMs = lv_screenGetTimeRunning();
    if (back) {
        known = (lv_screenRewindTargetMs(&toMs) == TRUE && toMs < nowMs);
    } else if (lv_screenSpecIsLiveMode()) {
        known = lvJumpLiveHeadMs(nowMs, &toMs);
    } else {
        known = (lv_screenFastForwardTargetMs(&toMs) == TRUE && toMs > nowMs);
    }
    steps = known ? lvJumpAnimSteps(nowMs, toMs, LV_JUMP_ANIM_STEPS, 0, UINT32_MAX) : 1;
    if (steps < 2) {
        lv_clientMutexRelease();
        return FALSE;
    }
    s_lvJump.active = TRUE;
    s_lvJump.back = back;
    s_lvJump.fromMs = nowMs;
    s_lvJump.toMs = toMs;
    s_lvJump.steps = steps;
    s_lvJump.shown = 0;
    s_lvJump.startNs = SDL_GetTicksNS();
    s_lvJump.serial = lv_screenSeekSerial();
    lv_windowSetTickHold(true);
    lv_clientMutexRelease();
    return TRUE;
}

/* One frame of the animation: seek to the step that is due, if it is a new
 * one. Called by lvHostRenderFrame before the frame is drawn. */
static void lvJumpAnimFrame(void) {
    uint32_t elapsedMs;
    int k;
    bool slow = FALSE;

    if (!s_lvJump.active) {
        return;
    }
    if (g_lv->isLoaded == FALSE) {
        lvJumpAnimDrop();
        return;
    }
    lv_clientMutexWaitFor();
    if (lv_screenSeekSerial() != s_lvJump.serial) {
        lv_clientMutexRelease();
        lvJumpAnimDrop();
        return;
    }
    elapsedMs = (uint32_t)((SDL_GetTicksNS() - s_lvJump.startNs) / 1000000u);
    k = lvJumpAnimStepAt(elapsedMs, LV_JUMP_ANIM_MS, s_lvJump.steps);
    if (k >= s_lvJump.steps) {
        lv_clientMutexRelease();
        lvJumpAnimFinish();
        return;
    }
    if (k > s_lvJump.shown) {
        uint32_t nowMs = lv_screenGetTimeRunning();
        uint32_t stepMs = lvJumpAnimStepMs(s_lvJump.fromMs, s_lvJump.toMs, k, s_lvJump.steps,
                                           0, UINT32_MAX);
        s_lvJump.shown = k;
        if (s_lvJump.back ? (stepMs < nowMs) : (stepMs > nowMs)) {
            Uint64 t0 = SDL_GetTicksNS();
            lv_drawDirtyScreen();
            lv_screenSeekToTimeMs(stepMs);
            lv_drawDirtyScreen();
            s_lvJump.serial = lv_screenSeekSerial();
            g_lv->wantScreenUpdate = TRUE;
            slow = (SDL_GetTicksNS() - t0) > (Uint64)LV_JUMP_ANIM_STEP_MAX_MS * 1000000u;
        }
    }
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
    /* A log with its snapshots far apart: a step is a long decode. The rest
     * of the jump is made now, as with no animation. */
    if (slow) {
        lvJumpAnimFinish();
    }
}

void lv_windowFastForward(void) {
    lvJumpAnimFinish();
    if (lvJumpAnimStart(FALSE)) {
        return;
    }
    lvWindowFastForwardNow();
}

void lv_windowRewind(void) {
    lvJumpAnimFinish();
    if (lvJumpAnimStart(TRUE)) {
        return;
    }
    lvWindowRewindNow(0, 0);
}

void lv_windowResize(void) {
    lv_clientMutexWaitFor();
    lv_drawResizeRenderTarget();
    lv_clientMutexRelease();
}

/* --------------------------------------------------------------------------
 * Start-of-log callback from backend
 * -------------------------------------------------------------------------- */
void lv_startOfLog(void) {
    /* ImGui controls panel reads isLoaded each frame */
}

/* --------------------------------------------------------------------------
 * Open / Save file
 * -------------------------------------------------------------------------- */
void lv_windowOpenFile(char *cmdLine) {
    char fileName[FILENAME_MAX];
    int  dlgResult;

    /* A live spectator must not tear down the delayed feed by opening another
       log. The menus have no Open in live mode and Ctrl+O does nothing there,
       but the check is kept at the action itself as well. Standalone playback
       and the command-line open are never in live mode. */
    if (lv_screenSpecIsLiveMode()) {
        return;
    }

    /* The new log wins over an animation of Rewind or Fast Forward. */
    lvJumpAnimDrop();
    lv_windowStop(FALSE);
    fileName[0] = '\0';

    if (cmdLine == NULL) {
        dlgResult = lv_platform_dialog_open_file(
            "Open File...", "WinBolo Log Files", "*.wbv", "wbv",
            fileName, sizeof(fileName), NULL);
    } else {
        dlgResult = PLATFORM_DIALOG_OK;
        strncpy(fileName, cmdLine, sizeof(fileName) - 1);
        fileName[sizeof(fileName) - 1] = '\0';
    }

    if (dlgResult == PLATFORM_DIALOG_OK) {
        snprintf(s_pendingOpenPath, sizeof(s_pendingOpenPath), "%s", fileName);
        s_pendingOpen = TRUE;
    }
}

/* Open a log held in memory (a WinBolo.net download) at the top of the next
 * frame, as lv_windowOpenFile does a file. Takes ownership of zipData. */
void lv_windowOpenMemory(uint8_t *zipData, size_t zipLen) {
    lv_windowStop(FALSE);
    free(s_pendingZipData);
    s_pendingZipData = zipData;
    s_pendingZipLen  = zipLen;
}

/* Begin the open lv_windowOpenFile or lv_windowOpenMemory asked for. */
static void lvHostLoadBegin(void) {
    char memoryBuff[32];
    bool begun;

    if (s_pendingOpen || s_pendingZipData != NULL) {
        /* A GIF being made is of the log that closes now. */
        lv_clip_gif_cancel();
    }
    if (s_pendingOpen) {
        s_pendingOpen = FALSE;
        lv_platform_config_get_string("LOGVIEWER", "Memory Buffer", "4",
                                   memoryBuff, sizeof(memoryBuff));
        begun = lv_screenLoadMapBegin(s_pendingOpenPath, atoi(memoryBuff));
        if (begun == FALSE) {
            lv_platform_dialog_error(DIALOG_BOX_TITLE, "Could not open log file");
        }
    } else if (s_pendingZipData != NULL) {
        uint8_t *zipData = s_pendingZipData;
        s_pendingZipData = NULL;
        /* Takes ownership even on failure. */
        begun = lv_screenLoadMapFromMemoryBegin(zipData, s_pendingZipLen);
        s_pendingZipLen = 0;
    } else {
        return;
    }
    g_lv->isLoaded = FALSE;
    s_loading = begun;
    s_loadStartMs = SDL_GetTicks();
}

/* Give the load in progress this frame's share of work. The log is loaded
 * once a step finishes it; a step with nothing to do means it was closed part
 * way, and it stays unloaded. */
static void lvHostLoadSteps(void) {
    Uint64 until;
    int    result;

    if (!s_loading) {
        return;
    }
    until = SDL_GetTicks() + LV_LOAD_FRAME_MS;
    lv_clientMutexWaitFor();
    do {
        result = lv_screenLoadStep();
    } while (result > 0 && SDL_GetTicks() < until);
    lv_clientMutexRelease();
    if (result <= 0) {
        s_loading = FALSE;
        if (result == 0) {
            g_lv->isLoaded = TRUE;
            lvStatsEmitRoundSummary();
            lv_windowCollectAllEvents();
            lv_windowNeedRedraw();
        }
    }
}

/* The progress window, once the load has run long enough to be worth one. */
static void lvHostDrawLoadProgress(void) {
    LvLoadStage stage;
    float       fraction;
    langid      text;

    if (!s_loading || SDL_GetTicks() - s_loadStartMs < LV_LOAD_PROGRESS_DELAY_MS) {
        return;
    }
    lv_screenLoadProgress(&stage, &fraction);
    switch (stage) {
    case LV_LOAD_UNZIP: text = STR_LV_LOAD_UNZIP; break;
    case LV_LOAD_SCAN:  text = STR_LV_LOAD_SCAN;  break;
    case LV_LOAD_PLAY:
    default:            text = STR_LV_LOAD_PLAY;  break;
    }
    lv_imgui_load_progress(langGetText(text), (int)stage + 1, 3, fraction);
}

void lv_windowSaveMap(void) {
    char fileName[FILENAME_MAX];
    char currentTime[FILENAME_MAX];
    int  dlgResult;
    int  count = 0;
    int  len;

    /* The live spectator never saves the delayed feed to a .wbv. The menus
       have no Save Map in live mode, and this does nothing there as well (it
       would otherwise freeze the feed via the pause below). Standalone is
       unaffected. */
    if (lv_screenSpecIsLiveMode()) {
        return;
    }

    lv_windowPause();
    fileName[0] = '\0';

    lv_clientMutexWaitFor();
    lv_screenGetMapName(fileName);
    lv_screenGetTime(currentTime);
    lv_clientMutexRelease();

    /* Replace ':' in time string so it can appear in filenames */
    len = (int)strlen(currentTime);
    while (count < len) {
        if (currentTime[count] == ':') currentTime[count] = '.';
        count++;
    }

    strncat(fileName, " - ", sizeof(fileName) - strlen(fileName) - 1);
    strncat(fileName, currentTime, sizeof(fileName) - strlen(fileName) - 1);

    dlgResult = lv_platform_dialog_save_file(
        "Save Map File...", "WinBolo Map Files", "*.MAP", "map",
        fileName, fileName, sizeof(fileName), NULL);

    if (dlgResult == PLATFORM_DIALOG_OK) {
        if (lv_screenSaveMap(fileName, FALSE) == FALSE) {
            lv_platform_dialog_error(DIALOG_BOX_TITLE, langGetText(STR_LV_ERR_SAVE_MAP));
        }
    }
}

/* --------------------------------------------------------------------------
 * File > Open map at this time in single player
 *
 * Writes the world at the playhead as a map file and starts the WinBolo game
 * beside this executable on it. The viewer stays open, paused.
 * -------------------------------------------------------------------------- */

/* The path of the WinBolo game that goes with this Log Viewer, or FALSE
 * when there is none.
 *
 * Windows and Linux: the game is the file beside this executable.
 *
 * macOS: both programs are application bundles, and they sit side by side
 * (the build folder, the disk image and the Applications folder all hold
 * them so). SDL_GetBasePath answers "Log Viewer.app/Contents/Resources/"
 * for a bundle, so the game is three folders up and then down into
 * "WinBolo.app/Contents/MacOS/WinBolo". A Log Viewer built without a bundle
 * has the game beside it as on Linux, so that place is tried second. */
static bool lvFindGameExe(char *out, size_t outSize) {
    const char *baseDir = SDL_GetBasePath();
    int         n;

    if (baseDir == NULL) {
        baseDir = "";
    }
#ifdef __APPLE__
    n = snprintf(out, outSize, "%s../../../WinBolo.app/Contents/MacOS/WinBolo", baseDir);
    if (n > 0 && (size_t)n < outSize && SDL_GetPathInfo(out, NULL)) {
        return TRUE;
    }
#endif
#ifdef _WIN32
    n = snprintf(out, outSize, "%sWinBolo.exe", baseDir);
#else
    n = snprintf(out, outSize, "%sWinBolo", baseDir);
#endif
    if (n > 0 && (size_t)n < outSize && SDL_GetPathInfo(out, NULL)) {
        return TRUE;
    }
    return FALSE;
}

/* Only the Log Viewer run as its own program offers it. The viewer opened
 * from the game's menu is inside the game, and a live spectator has no
 * whole world to hand over.
 *
 * On macOS the menu item is also off when the game is not found. The place
 * the game is looked for there has not been tried on a Mac, and macOS can
 * run a downloaded bundle from a private copy with nothing beside it, so a
 * greyed item is better than an item that always ends in an error. The
 * look-up is done once: the menu asks every frame. */
bool lv_windowCanOpenSinglePlayer(void) {
#ifdef __APPLE__
    static int s_gameFound = -1;
    if (s_gameFound < 0) {
        char exePath[FILENAME_MAX];
        s_gameFound = lvFindGameExe(exePath, sizeof(exePath)) ? 1 : 0;
    }
    if (s_gameFound == 0) {
        return FALSE;
    }
#endif
    return (g_lv != NULL && g_lv->isLoaded && !g_lv->fromMainMenu &&
            !lv_screenSpecIsLiveMode()) ? TRUE : FALSE;
}

void lv_windowOpenSinglePlayer(void) {
    char         mapPath[FILENAME_MAX];
    char         exePath[FILENAME_MAX];
    char         mapName[FILENAME_MAX];
    char         currentTime[FILENAME_MAX];
    char        *prefDir;
    const char  *args[4];
    SDL_Process *game = NULL;
    SDL_PropertiesID props;
    LvSpmapInfo  info;
    int          self;
    int          wrote;
    int          n;
    int          attempt;
    size_t       i, j;
    int          follow;

    if (lv_windowCanOpenSinglePlayer() == FALSE) {
        return;
    }
    lv_windowPause();

    /* The game first: with no game to start there is no reason to write a
       map file. */
    if (!lvFindGameExe(exePath, sizeof(exePath))) {
        lv_platform_dialog_error(DIALOG_BOX_TITLE,
                                 langGetText(STR_LV_ERR_NO_GAME));
        return;
    }

    /* The map goes with the game's own files (where it keeps its logs and
       preferences), in a folder of its own, never in data/maps. The name is
       the map's and the time, so the game's lobby says what it is. */
    prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir == NULL) {
        lv_platform_dialog_error(DIALOG_BOX_TITLE, langGetText(STR_LV_ERR_SAVE_MAP));
        return;
    }
    n = snprintf(mapPath, sizeof(mapPath), "%slogviewer-maps", prefDir);
    /* SDL_CreateDirectory is true for a folder that is already there. */
    if (n <= 0 || (size_t)n >= sizeof(mapPath) || !SDL_CreateDirectory(mapPath)) {
        SDL_free(prefDir);
        lv_platform_dialog_error(DIALOG_BOX_TITLE,
                                 langGetText(STR_LV_ERR_MAP_FOLDER));
        return;
    }

    mapName[0]     = '\0';
    currentTime[0] = '\0';
    lv_clientMutexWaitFor();
    lv_screenGetMapName(mapName);
    lv_screenGetTime(currentTime);
    lv_clientMutexRelease();
    strncat(mapName, " - ", sizeof(mapName) - strlen(mapName) - 1);
    strncat(mapName, currentTime, sizeof(mapName) - strlen(mapName) - 1);
    /* Nothing a file name cannot hold (the time has ':' in it). The game
       opens the -spmap file with the C library's fopen, which on Windows
       reads the path in the system's code page and not as UTF-8. The game
       changes the folder part to a form that fopen opens (winboloSpMapPath
       in src/gui/sdl3/winbolo.c), and to make that sure the file name is
       ASCII only: each letter outside ASCII becomes one '_'.
       The lead byte of the letter gives the '_' and the continuation bytes
       (0x80..0xBF) that belong to it are dropped. A continuation byte with
       no lead byte before it is not part of a letter, so it gives a '_' of
       its own. */
    for (i = 0, j = 0, follow = 0; mapName[i] != '\0'; i++) {
        unsigned char c = (unsigned char)mapName[i];
        if (c >= 0x80 && c <= 0xBF && follow > 0) {
            follow--;
            continue;
        }
        follow = 0;
        if (c >= 0x80) {
            if (c >= 0xC0 && c <= 0xDF) {
                follow = 1;
            } else if (c >= 0xE0 && c <= 0xEF) {
                follow = 2;
            } else if (c >= 0xF0 && c <= 0xF7) {
                follow = 3;
            }
            c = '_';
        } else if (strchr("\\/:*?\"<>|", (char)c) != NULL) {
            c = '.';
        }
        mapName[j++] = (char)c;
    }
    mapName[j] = '\0';
    /* A name no file has yet. The map name and the playback time are the
       same for two logs of one map, and for a second try at the same time;
       the game that was started on the first file may still have it open.
       So a file is never written over: the second gets " (2)", and so on.
       The file is made by the open itself (lvSpmapWriteNew), which fails
       when a file of that name is there, so no other program can make the
       file between a check and the write.

       The selected player (the perspective the HUD or Select Team set) is
       the side the user takes. With none, nothing is the user's. */
    wrote = 0;
    lv_clientMutexWaitFor();
    self = lv_playersHasPerspective() ? (int)lv_playersGetSelf() : -1;
    for (attempt = 1; attempt <= 9999; attempt++) {
        if (attempt == 1) {
            n = snprintf(mapPath, sizeof(mapPath), "%slogviewer-maps/%.200s.map",
                         prefDir, mapName);
        } else {
            n = snprintf(mapPath, sizeof(mapPath), "%slogviewer-maps/%.200s (%d).map",
                         prefDir, mapName, attempt);
        }
        if (n <= 0 || (size_t)n >= sizeof(mapPath)) {
            break;
        }
        wrote = lvSpmapWriteNew(mapPath, self, &info);
        if (wrote != LV_SPMAP_EXISTS) {
            break;
        }
    }
    lv_clientMutexRelease();
    SDL_free(prefDir);
    if (wrote != 1) {
        lv_platform_dialog_error(DIALOG_BOX_TITLE, langGetText(STR_LV_ERR_SAVE_MAP));
        return;
    }

    args[0] = exePath;
    args[1] = "-spmap";
    args[2] = mapPath;
    args[3] = NULL;
    /* Started as a background process: the viewer never waits for the game.
       On macOS and Linux a child that is not waited for stays in the process
       table as a zombie after it ends, until the viewer itself ends. SDL
       starts a background process through a middle process that ends at
       once, so the game is nobody's child and leaves nothing behind. On
       Windows there is no such thing and this changes nothing but the
       game's standard input and output, which go nowhere. */
    props = SDL_CreateProperties();
    if (props != 0) {
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
        game = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
    }
    if (game == NULL) {
        lv_platform_dialog_error(DIALOG_BOX_TITLE, langGetText(STR_LV_ERR_START_GAME));
        return;
    }
    /* Lets go of our handle on it; the game runs on. */
    SDL_DestroyProcess(game);
}

/* --------------------------------------------------------------------------
 * File > Save log file to disk
 *
 * Saves a copy of the open log's own file: the .wbv it was opened from, or
 * the .wbv bytes a download handed over.
 * -------------------------------------------------------------------------- */

/* Whether two paths spell the same file: slashes either way, and no case on
 * Windows. A file reached by two different spellings is not caught here; the
 * save reads the whole source before it writes, so that still copies right. */
static bool lvSamePath(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        char ca = (*a == '\\') ? '/' : *a;
        char cb = (*b == '\\') ? '/' : *b;
#ifdef _WIN32
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
#endif
        if (ca != cb) {
            return FALSE;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0') ? TRUE : FALSE;
}

bool lv_windowCanSaveLog(void) {
    if (g_lv == NULL || !g_lv->isLoaded || lv_screenSpecIsLiveMode()) {
        return FALSE;
    }
    return (lv_blocksGetSource(NULL, NULL, NULL) != LV_LOG_SOURCE_NONE) ? TRUE : FALSE;
}

/* The name offered for the saved copy: the game's date, its map and its
 * players (lv_save_name.h), not the log's own file name, which is often a
 * key of letters and numbers. Everything is read here, so the caller holds
 * the decoder's mutex. srcPath is the log's own file, or "" for a download.
 *
 * The date is the best one there is, in this order:
 *   1. the stamp at the start of the file's own name, when the server named
 *      the file: the time that round's log was opened;
 *   2. the create time in the log's header. It is the time the server was
 *      started, not the round: right for a game hosted and played at once,
 *      early for a server that has been up for days;
 *   3. for a file, the time it was last written.
 * A download with none of them has no date in its name. */
static void lvSaveLogDefaultName(const char *srcPath, char *out, size_t outSize) {
    static LvRosterRow rows[LV_ROSTER_MAX]; /* main thread only */
    const char        *names[LV_ROSTER_MAX];
    unsigned char      teams[LV_ROSTER_MAX];
    unsigned char      bots[LV_ROSTER_MAX];
    char               mapName[FILENAME_MAX];
    const char        *base = srcPath;
    const char        *p;
    int                year = 0, month = 0, day = 0, hour = -1, minute = 0;
    int                count, i;
    LvSaveNameWords    words;
    int32_t            created;

    for (p = srcPath; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    created = lv_screenGetCreateTime();
    if (lvSaveNameStamp(base, &year, &month, &day, &hour, &minute)) {
        /* 1: the parts are set */
    } else if (created > 0) {
        time_t     t  = (time_t)created;
        struct tm *tm = localtime(&t);
        if (tm != NULL) {
            year   = tm->tm_year + 1900;
            month  = tm->tm_mon + 1;
            day    = tm->tm_mday;
            hour   = tm->tm_hour;
            minute = tm->tm_min;
        }
    } else if (srcPath[0] != '\0') {
        SDL_PathInfo info;
        SDL_DateTime dt;
        if (SDL_GetPathInfo(srcPath, &info) &&
            SDL_TimeToDateTime(info.modify_time, &dt, true)) {
            year   = dt.year;
            month  = dt.month;
            day    = dt.day;
            hour   = dt.hour;
            minute = dt.minute;
        }
    }

    mapName[0] = '\0';
    lv_screenGetMapName(mapName);
    count = lv_screenGetRoster(rows, LV_ROSTER_MAX);
    for (i = 0; i < count; i++) {
        names[i] = rows[i].name;
        teams[i] = rows[i].team;
        bots[i]  = rows[i].bot;
    }
    words.vs   = langGetText(STR_LV_SAVE_NAME_VS);
    words.more = langGetText(STR_LV_SAVE_NAME_MORE);
    words.bot  = langGetText(STR_LV_SAVE_NAME_BOT);
    words.bots = langGetText(STR_LV_SAVE_NAME_BOTS);
    words.log  = langGetText(STR_LV_SAVE_NAME_LOG);
    lvSaveNameBuildWords(out, outSize, year, month, day, hour, minute,
                         mapName, names, teams, bots, count, &words);
}

void lv_windowSaveLog(void) {
    char           srcPath[FILENAME_MAX];
    char           defaultName[FILENAME_MAX];
    char           fileName[FILENAME_MAX];
    const char    *path = NULL;
    const uint8_t *data = NULL;
    uint8_t       *copy = NULL;
    size_t         len  = 0;
    char          *prefDir;
    FILE          *fp;
    int            source;
    int            dlgResult;
    bool           ok;
    const char    *failed = langGetText(STR_LV_ERR_SAVE_LOG);

    if (lv_windowCanSaveLog() == FALSE) {
        return;
    }
    lv_windowPause();

    /* Everything the save needs from the decoder is taken here, under the
       mutex, and nothing of the decoder's is touched after it is released:
       the dialog below can stay open for minutes, and the bytes of a
       downloaded log belong to the decoder, which frees them when another
       log is loaded. */
    srcPath[0]     = '\0';
    defaultName[0] = '\0';
    fileName[0]    = '\0';
    ok = TRUE;
    lv_clientMutexWaitFor();
    source = lv_blocksGetSource(&path, &data, &len);
    if (source == LV_LOG_SOURCE_FILE) {
        snprintf(srcPath, sizeof(srcPath), "%s", path);
    } else if (source == LV_LOG_SOURCE_MEMORY) {
        /* A downloaded log has no file. Its bytes are copied now. */
        copy = (uint8_t *)malloc(len);
        if (copy != NULL) {
            memcpy(copy, data, len);
        } else {
            ok = FALSE;
        }
    } else {
        ok = FALSE;
    }
    if (ok) {
        lvSaveLogDefaultName(srcPath, defaultName, sizeof(defaultName));
    }
    lv_clientMutexRelease();
    data = NULL;
    path = NULL;
    if (!ok) {
        free(copy);
        lv_platform_dialog_error(DIALOG_BOX_TITLE, failed);
        return;
    }

    /* Opens on the folder the game keeps its logs in. The name offered and
       the path that comes back are two buffers: the dialog copies the one
       into the other. */
    prefDir   = SDL_GetPrefPath("WinBolo", "WinBolo");
    dlgResult = lv_platform_dialog_save_file(
        langGetText(STR_LV_SAVE_LOG_TITLE), langGetText(STR_DLGWBN_FILEFILTER),
        "*.wbv", "wbv",
        defaultName, fileName, sizeof(fileName), prefDir);
    SDL_free(prefDir);
    if (dlgResult != PLATFORM_DIALOG_OK) {
        free(copy);
        if (dlgResult == PLATFORM_DIALOG_ERROR) {
            /* No dialog, or a path too long for fileName: nothing was
               saved, and the user is told. */
            lv_platform_dialog_error(DIALOG_BOX_TITLE, failed);
        }
        return;
    }
    if (source == LV_LOG_SOURCE_FILE && lvSamePath(srcPath, fileName)) {
        return;   /* it is already there */
    }

    if (source == LV_LOG_SOURCE_FILE) {
        /* All of the source first, then the write: right even if the two
           names turn out to be one file. */
        long size = 0;
        fp = lvFopenUtf8(srcPath, "rb");
        ok = (fp != NULL);
        if (ok) {
            ok = fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) > 0 &&
                 fseek(fp, 0, SEEK_SET) == 0;
        }
        if (ok) {
            len  = (size_t)size;
            copy = (uint8_t *)malloc(len);
            ok   = (copy != NULL) && fread(copy, 1, len, fp) == len;
        }
        if (fp != NULL) {
            fclose(fp);
        }
        if (!ok) {
            failed = langGetText(STR_LV_ERR_LOG_GONE);
        }
    }
    if (ok) {
        /* The dialog's path is UTF-8, on Windows too. */
        fp = lvFopenUtf8(fileName, "wb");
        ok = (fp != NULL);
        if (ok) {
            ok = fwrite(copy, 1, len, fp) == len;
            if (fclose(fp) != 0) {
                ok = FALSE;
            }
        }
    }
    free(copy);
    if (!ok) {
        lv_platform_dialog_error(DIALOG_BOX_TITLE, failed);
    }
}

/* --------------------------------------------------------------------------
 * Preferences
 * -------------------------------------------------------------------------- */
static void loadPreferences(void) {
    char line[FILENAME_MAX];
    char def[FILENAME_MAX];
    char val[FILENAME_MAX];
    bool dns;
    BYTE count;

    /* Tank Centred */
    lv_platform_config_get_string("LOGVIEWER", "Tank Centred", "No", line, sizeof(line));
    if (tolower((unsigned char)line[0]) == 'y') {
        lv_screenTankCentred(TRUE);
    } else {
        lv_screenTankCentred(FALSE);
    }

    /* Sound Effects */
    lv_platform_config_get_string("LOGVIEWER", "Sounds", "Yes", line, sizeof(line));
    g_lv->isSoundsPlaying = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* Sound Volume (0-100, mirrors main game) */
    lv_platform_config_get_string("LOGVIEWER", "Sound Volume", "50", line, sizeof(line));
    g_lv->soundVolume = atoi(line);
    if (g_lv->soundVolume < 0) g_lv->soundVolume = 0;
    if (g_lv->soundVolume > 100) g_lv->soundVolume = 100;
    lv_soundSetVolume(g_lv->soundVolume);

    /* DNS Lookups */
    lv_platform_config_get_string("LOGVIEWER", "DNS Lookups", "No", line, sizeof(line));
    dns = (tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;
    if (dns == TRUE) {
        dns = (bool)lv_dnsSetEnabled(TRUE);
    }

    /* Use Team Colours */
    lv_platform_config_get_string("LOGVIEWER", "Use Team Colours", "Yes", line, sizeof(line));
    g_lv->useTeamColours = (line[0] == '\0' || tolower((unsigned char)line[0]) == 'y') ? TRUE : FALSE;

    /* Team colours. A stored old default with no mark reads as the new
       default (lvTeamColourStoredPref). */
    {
        int marked;
        lv_platform_config_get_string("LOGVIEWER", LV_TEAM_COLOUR_MARK_KEY, "", val, sizeof(val));
        marked = lvTeamColourMarked(val);
        count = 0;
        while (count < MAX_TANKS) {
            snprintf(line, sizeof(line), "Team Colour %d", count + 1);
            getDef(def, count);
            lv_platform_config_get_string("LOGVIEWER", line, def, val, sizeof(val));
            g_lv->tc[count] = lvTeamColourStoredPref(count, atoi(val), marked);
            count++;
        }
        snprintf(def, 12, "%d", (int)lvTeamColourDefault(LV_TEAM_COLOUR_NEUTRAL));
        lv_platform_config_get_string("LOGVIEWER", LV_TEAM_COLOUR_NEUTRAL_KEY, def, val, sizeof(val));
        g_lv->tc[16] = lvTeamColourStoredPref(LV_TEAM_COLOUR_NEUTRAL, atoi(val), marked);
    }

    /* The simplified view, from the game's own [SETTINGS] section rather than
       a key of this viewer's. Both read the same preferences document, and a
       player who has asked for a readable zoomed-out map has asked once.

       The game's sub-option, SimplifiedOverviewOnly, is not read: it chooses
       between the Map Overview window and the full screen map, and this
       viewer is neither. */
    lv_platform_config_get_string("SETTINGS", "SimplifiedZoomOut", "Yes",
                                  line, sizeof(line));
    gfxSetSimplifiedZoomOut(tolower((unsigned char)line[0]) != 'n');

    /* Playback Speed */
    lv_platform_config_get_string("LOGVIEWER", "Playback Speed", "1", val, sizeof(val));
    {
        BYTE spd = (BYTE)atoi(val);
        if (spd < 1 || spd > LV_SPEED_MAX) spd = 1;
        lv_updateSpeed(spd, 0);
    }
}

bool lv_windowCanFullScreen(void);

/* The drawing area the command line asked for with -size, in pixels, or 0
 * for none (logViewerSetWindowSizeText). While one is set the window stays
 * that size: it is not snapped to whole squares, cannot be resized by hand,
 * and neither its size nor its place is saved, so the next run without -size
 * opens as the last one without it closed. */
static int  s_fixedWindowW = 0;
static int  s_fixedWindowH = 0;
/* The text after -size was not a size (lvHostWarnFixedWindowSize). */
static bool s_fixedWindowRefused = false;
/* The size the window came out at, in pixels, once it was applied. */
static int  s_fixedWindowGotW = 0;
static int  s_fixedWindowGotH = 0;

void logViewerSetWindowSizeText(const char *text) {
    int w = 0, h = 0;

    if (text != NULL && lvWindowSizeParse(text, &w, &h)) {
        s_fixedWindowW = w;
        s_fixedWindowH = h;
        s_fixedWindowRefused = false;
    } else {
        s_fixedWindowW = 0;
        s_fixedWindowH = 0;
        s_fixedWindowRefused = true;
    }
}

static bool lvHostFixedWindowSize(void) {
    return s_fixedWindowW > 0 && s_fixedWindowH > 0 && g_lv != NULL && g_lv->ownsWindow;
}

static void savePreferences(void) {
    char line[256];
    char val[64];
    BYTE count;

    /* ImGui module save (mode, dns, tank centred, window visibility) */
    lv_imgui_main_menu_save();
    lv_imgui_dialogs_save();

    /* Sound Effects */
    lv_platform_config_set_string("LOGVIEWER", "Sounds", g_lv->isSoundsPlaying ? "Yes" : "No");

    /* Sound Volume */
    snprintf(val, sizeof(val), "%d", g_lv->soundVolume);
    lv_platform_config_set_string("LOGVIEWER", "Sound Volume", val);

    /* Use Team Colours */
    lv_platform_config_set_string("LOGVIEWER", "Use Team Colours", g_lv->useTeamColours ? "Yes" : "No");

    /* Screen Size. The next launch makes its window this many squares wide
     * and high, so while a sidebar takes part of the window the number saved
     * is the whole window's, not the map's: saving the map's would make the
     * window smaller by the sidebar's size at every launch.
     *
     * A maximized or fullscreen window is not a size to start the next run
     * at: the sizes saved before it was maximized stay as they are, so the
     * window that is maximized again at the next start (Window.Maximized)
     * has its own size to go back to.
     *
     * Only the viewer's own window is a size to start it at. The viewer
     * inside the game (Watch a Log, spectating) draws in the game's window,
     * whose size is the game's; written here it would be the size the
     * standalone viewer opens at next time. */
    {
        SDL_WindowFlags wflags = SDL_GetWindowFlags(g_lv->window);
        bool big = (wflags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0;
        bool fixed = lvHostFixedWindowSize();
        bool placed = g_lv->ownsWindow && !big && !fixed && !(wflags & SDL_WINDOW_MINIMIZED);

        if (g_lv->ownsWindow && !big && !fixed) {
            /* The next launch makes the window sizeX * TILE_SIZE_X points
             * wide (draw.c), at zoom 1, so the count saved is the window's
             * in whole squares at zoom 1. The grid itself
             * (lv_screenGetSizeX) is not that: it is sized against the
             * zoom, so at 50% it holds twice the squares the window has. */
            int sizeX = lv_screenGetSizeX();
            int sizeY = lv_screenGetSizeY();
            int winW = 0, winH = 0;
            int menuH = (int)lv_imgui_get_menu_bar_height();
            SDL_GetWindowSize(g_lv->window, &winW, &winH);
            if (winW > 0) {
                sizeX = lvFitTilesNearest(winW, (float)TILE_SIZE_X);
            }
            if (winH > menuH) {
                sizeY = lvFitTilesNearest(winH - menuH, (float)TILE_SIZE_Y);
            }
            snprintf(line, sizeof(line), "%d", sizeX);
            lv_platform_config_set_string("LOGVIEWER", "ScreenSizeX", line);
            snprintf(line, sizeof(line), "%d", sizeY);
            lv_platform_config_set_string("LOGVIEWER", "ScreenSizeY", line);
        }

        /* Where the viewer's own window is, and its exact size: the squares
         * above are whole squares, which is not exact enough to bring a
         * layout back as it was. Not written for WinBolo's window, which the
         * viewer only borrows. */
        /* Full screen is saved as the game saves its own ([SETTINGS]
         * FullScreen there), so the viewer opens the way it was closed.
         * While the window is full screen "maximized" is left as it was
         * saved: that is the state the window goes back to. */
        if (g_lv->ownsWindow && !fixed) {
            if (!(wflags & SDL_WINDOW_FULLSCREEN)) {
                lv_platform_config_set_string("LOGVIEWER", "Window.Maximized",
                                              (wflags & SDL_WINDOW_MAXIMIZED) ? "Yes" : "No");
            }
            lv_platform_config_set_string("LOGVIEWER", "Window.FullScreen",
                                          (wflags & SDL_WINDOW_FULLSCREEN) ? "Yes" : "No");
        }
        if (placed) {
            int x = 0, y = 0, w = 0, h = 0;
            SDL_GetWindowPosition(g_lv->window, &x, &y);
            SDL_GetWindowSize(g_lv->window, &w, &h);
            if (w > 0 && h > 0) {
                lv_platform_config_set_int("LOGVIEWER", "Window.X", x);
                lv_platform_config_set_int("LOGVIEWER", "Window.Y", y);
                lv_platform_config_set_int("LOGVIEWER", "Window.W", w);
                lv_platform_config_set_int("LOGVIEWER", "Window.H", h);
            }
        }
    }

    /* Team colours */
    count = 0;
    while (count < MAX_TANKS) {
        snprintf(line, sizeof(line), "Team Colour %d", count + 1);
        snprintf(val, sizeof(val), "%d", g_lv->tc[count]);
        lv_platform_config_set_string("LOGVIEWER", line, val);
        count++;
    }
    snprintf(val, sizeof(val), "%d", g_lv->tc[16]);
    lv_platform_config_set_string("LOGVIEWER", LV_TEAM_COLOUR_NEUTRAL_KEY, val);
    lv_platform_config_set_string("LOGVIEWER", LV_TEAM_COLOUR_MARK_KEY, LV_TEAM_COLOUR_MARK);

    /* Playback speed */
    snprintf(val, sizeof(val), "%d", g_lv->speed);
    lv_platform_config_set_string("LOGVIEWER", "Playback Speed", val);

    lv_platform_config_set_string("LOGVIEWER", "PreferencesSaved", "Yes");
    lv_platform_config_save();
}

/* Stands for "no value saved": no window is ever placed here. */
#define LV_WINDOW_PLACE_NONE (-1000000)

/* --------------------------------------------------------------------------
 * lvHostApplyFixedWindowSize -- Give the window the size -size asked for.
 *
 * The size is in pixels, but SDL sizes a window in its own units, which are
 * pixels divided by the pixel density; the density is only certain once the
 * window is on its display, so the size is set, read back in pixels, and set
 * once more if it came out wrong. The window is centred on its display and
 * opens neither maximized nor full screen, whatever the last run saved.
 * -------------------------------------------------------------------------- */
static void lvHostApplyFixedWindowSize(void) {
    int pw = 0, ph = 0, i;

    SDL_SetWindowResizable(g_lv->window, false);
    for (i = 0; i < 2; i++) {
        float density = SDL_GetWindowPixelDensity(g_lv->window);
        int unitsW = lvWindowSizeUnits(s_fixedWindowW, density);
        int unitsH = lvWindowSizeUnits(s_fixedWindowH, density);
        SDL_SetWindowSize(g_lv->window, unitsW, unitsH);
        SDL_SetWindowPosition(g_lv->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_SyncWindow(g_lv->window);
        SDL_GetWindowSizeInPixels(g_lv->window, &pw, &ph);
        if (pw == s_fixedWindowW && ph == s_fixedWindowH) {
            break;
        }
    }
    s_fixedWindowGotW = pw;
    s_fixedWindowGotH = ph;
    SDL_LogInfo(WB_LOG_CAT_LOGVIEWER, "-size %dx%d: the window is %dx%d pixels",
                s_fixedWindowW, s_fixedWindowH, pw, ph);
}

/* --------------------------------------------------------------------------
 * lvHostWarnFixedWindowSize -- Tell the user when -size did not get its way.
 *
 * Either the text after -size was not a size, so the window opened at its
 * usual size, or the window could not be made the size asked for, as when
 * the display is too small to hold it, and the recording would come out at
 * another size. Shown once the window is up, after lvHostRestoreWindowPlace,
 * so the text comes from the language loaded.
 * -------------------------------------------------------------------------- */
static void lvHostWarnFixedWindowSize(void) {
    if (g_lv == NULL || !g_lv->ownsWindow) {
        return;
    }
    if (s_fixedWindowRefused) {
        lv_platform_dialog_message(DIALOG_BOX_TITLE, langGetText(STR_LV_SIZE_ARG_BAD));
    } else if (lvHostFixedWindowSize() &&
               (s_fixedWindowGotW != s_fixedWindowW || s_fixedWindowGotH != s_fixedWindowH)) {
        MessageArgs args = {0};
        SDL_snprintf(args.string1, sizeof(args.string1), "%dx%d", s_fixedWindowW, s_fixedWindowH);
        SDL_snprintf(args.string2, sizeof(args.string2), "%dx%d", s_fixedWindowGotW, s_fixedWindowGotH);
        lv_platform_dialog_message(DIALOG_BOX_TITLE, langGetTextFmt(STR_LV_SIZE_NOT_MET, &args));
    }
}

/* --------------------------------------------------------------------------
 * lvHostRestoreWindowPlace -- Put the viewer's own window back where it was.
 *
 * Size, position and the maximized state from the last run. Called once the
 * window exists and before the first frame is drawn. What was saved is not
 * trusted: a size is kept inside the display it lands on, and a position is
 * used only when the top of the window is on a display that is connected
 * now, so a window last left on a screen that has since been unplugged
 * opens where SDL would have put it.
 * -------------------------------------------------------------------------- */
static void lvHostRestoreWindowPlace(void) {
    char val[16];
    int x, y, w, h;
    bool placed = FALSE;

    if (g_lv == NULL || !g_lv->ownsWindow || g_lv->window == NULL) {
        return;
    }
    if (lvHostFixedWindowSize()) {
        lvHostApplyFixedWindowSize();
        return;
    }
    x = lv_platform_config_get_int("LOGVIEWER", "Window.X", LV_WINDOW_PLACE_NONE);
    y = lv_platform_config_get_int("LOGVIEWER", "Window.Y", LV_WINDOW_PLACE_NONE);
    w = lv_platform_config_get_int("LOGVIEWER", "Window.W", 0);
    h = lv_platform_config_get_int("LOGVIEWER", "Window.H", 0);

    if (x != LV_WINDOW_PLACE_NONE && y != LV_WINDOW_PLACE_NONE && w > 0 && h > 0) {
        int count = 0, i;
        SDL_DisplayID *displays = SDL_GetDisplays(&count);
        for (i = 0; displays != NULL && i < count && !placed; i++) {
            SDL_Rect bounds, top, overlap;
            if (!SDL_GetDisplayUsableBounds(displays[i], &bounds)) {
                continue;
            }
            /* The strip a title bar would be dragged by. */
            top.x = x; top.y = y; top.w = w; top.h = 32;
            if (SDL_GetRectIntersection(&top, &bounds, &overlap) &&
                overlap.w >= 100 && overlap.h >= 16) {
                if (w > bounds.w) w = bounds.w;
                if (h > bounds.h) h = bounds.h;
                if (w >= 200 && h >= 150) {
                    SDL_SetWindowSize(g_lv->window, w, h);
                    SDL_SetWindowPosition(g_lv->window, x, y);
                    placed = TRUE;
                }
            }
        }
        SDL_free(displays);
    }

    lv_platform_config_get_string("LOGVIEWER", "Window.Maximized", "No", val, sizeof(val));
    if (val[0] == 'Y' || val[0] == 'y') {
        SDL_MaximizeWindow(g_lv->window);
    }
    /* Full screen last, so the size and place above are what the window has
     * when full screen is turned off. */
    lv_platform_config_get_string("LOGVIEWER", "Window.FullScreen", "No", val, sizeof(val));
    if ((val[0] == 'Y' || val[0] == 'y') && lv_windowCanFullScreen()) {
        SDL_SetWindowFullscreen(g_lv->window, true);
        SDL_SyncWindow(g_lv->window);
    }
    /* The resize this causes reaches the event loop before the first frame,
     * where lvHostHandleResize fits the map to the window. */
}

/* --------------------------------------------------------------------------
 * lvHostSetup -- Shared host bring-up for the log viewer and the spectator.
 *
 * The ImGui-free half (lvHostSetupCore) plus the viewer's own ImGui context and
 * panels, sound, preferences, and the screen sizing that fills the window.
 * Returns TRUE on success; on any failure it unwinds whatever it brought up and
 * returns FALSE with g_lv cleared.
 * -------------------------------------------------------------------------- */
static int lvHostSetup(SDL_Window *window, SDL_Renderer *renderer,
                       bool fromMainMenu) {
    if (lvHostSetupCore(window, renderer, fromMainMenu) == FALSE) {
        return FALSE;
    }

    /* Initialize ImGui */
    if (lv_imgui_context_init(g_lv->window, g_lv->renderer) == 0) {
        lv_drawCleanup();
        lv_clientMutexDestroy();
        free(g_lv);
        g_lv = NULL;
        return FALSE;
    }
    lv_imgui_main_menu_init(g_lv);
    lvHostDockReset();
#ifdef __APPLE__
    /* Install the native NSMenu after ImGui + window are up. The shim's
     * save-on-install / restore-on-uninstall stack swaps WinBolo's menu
     * out when embedded, and builds a Log Viewer app menu when standalone
     * (no previous mainMenu to save). */
    lv_mac_menubar_install(g_lv->window, g_lv);
#endif
    lv_imgui_controls_init(g_lv);
    lv_imgui_game_info_init();
    lv_imgui_events_init();
    lv_imgui_item_info_init();
    lv_imgui_comments_init();
    lv_imgui_dialogs_init(g_lv);
    lv_imgui_game_viewport_init(g_lv);
    /* No GIF from a viewer before, and the Clips window starts closed. */
    lv_clip_gif_session_end();

    /* Sound (non-fatal if it fails) */
    if (lv_soundSetup() == FALSE) {
        g_lv->isSoundsPlaying = FALSE;
    }

    /* Load preferences */
    loadPreferences();

    /* Sync screen tile size to actual window dimensions so the game view
     * fills the window on first frame (not just after a manual resize).
     * Tile counts are sized against the zoom-effective tile pixel
     * size (TILE_SIZE * zoom), so this stays correct if a zoom level
     * was restored from preferences. */
    {
        int initW, initH;
        SDL_GetWindowSize(g_lv->window, &initW, &initH);
        if (initW > 0 && initH > 0) {
            int menuH = (int)lv_imgui_get_menu_bar_height();
            float zoom = lv_drawGetZoomLevel();
            if (zoom <= 0.0f) zoom = 1.0f;
            float tilePxX = (float)TILE_SIZE_X * zoom;
            float tilePxY = (float)TILE_SIZE_Y * zoom;
            int newTilesX = (int)(((float)initW + tilePxX * 0.5f) / tilePxX);
            int newTilesY = (int)((((float)(initH - menuH)) + tilePxY * 0.5f) / tilePxY);
            if (newTilesX < 1) newTilesX = 1;
            if (newTilesY < 1) newTilesY = 1;
            if (newTilesX > 255) newTilesX = 255;
            if (newTilesY > 255) newTilesY = 255;

            if ((BYTE)newTilesX != lv_screenGetSizeX() || (BYTE)newTilesY != lv_screenGetSizeY()) {
                lv_screenSetSizeX((BYTE)newTilesX);
                lv_screenSetSizeY((BYTE)newTilesY);
                lv_drawResizeRenderTarget();
                lv_drawDirtyScreen();
            }
        }
    }

    return TRUE;
}

/* --------------------------------------------------------------------------
 * lvHostTeardown -- Shared host shutdown for the log viewer and the spectator.
 *
 * Stops the decoder, tears down ImGui / draw / sound / mutex / DNS and (when
 * standalone) the platform layer, then frees g_lv. The caller restores any
 * active game view and saves preferences first.
 * -------------------------------------------------------------------------- */
static void lvHostTeardown(void) {
    /* Drops a GIF being made and the Clips window's textures, which are made
     * on this renderer. */
    lv_clip_gif_session_end();
    lvHostTeardownCommon(true);
    free(g_lv);
    g_lv = NULL;
}

/* --------------------------------------------------------------------------
 * Full screen
 *
 * Alt+Enter, and the Full Screen item of the Windows menu, as in the game.
 * It is the same kind of full screen the game has: SDL_SetWindowFullscreen
 * with no display mode chosen, which is a borderless window over the whole
 * of the display the window is on. SDL keeps the size and place the window
 * had and puts them back when full screen is turned off.
 *
 * The viewer's own window: the state is saved with the window's place
 * (Window.FullScreen) and comes back at the next start, as the game's does.
 *
 * WinBolo's window (Watch a Log, a live game watched from the game): the
 * window and its full screen setting are the game's, and the game is not
 * reading the keyboard while the viewer runs. So the viewer asks the game to
 * do it (lvGameFullScreenChoose in lv_fullscreen_game.c, which calls
 * windowFullScreenChoose, what Alt+Enter does on the game's own screens
 * outside a game). The game's setting and the window then agree when the
 * viewer hands the window back, and the game saves the setting itself. The
 * game's front end has no code for this.
 *
 * One keystroke is one change. The viewer takes the event off SDL's queue
 * and gives it to nothing else, and the game reads no events while the
 * viewer runs, so the game cannot act on the same keystroke. As a second
 * guard the viewer does not ask twice within LV_FULLSCREEN_GUARD_MS.
 *
 * Not on a Steam Deck or a tablet: the window there is full screen from the
 * start and has no other size to go back to. The game has the same rule.
 * -------------------------------------------------------------------------- */
SDL_COMPILE_TIME_ASSERT(lv_fskey_return, LV_FSKEY_SCANCODE_RETURN == SDL_SCANCODE_RETURN);
SDL_COMPILE_TIME_ASSERT(lv_fskey_kp_enter, LV_FSKEY_SCANCODE_KP_ENTER == SDL_SCANCODE_KP_ENTER);
SDL_COMPILE_TIME_ASSERT(lv_fskey_lalt, LV_FSKEY_MOD_LALT == SDL_KMOD_LALT);
SDL_COMPILE_TIME_ASSERT(lv_fskey_ralt, LV_FSKEY_MOD_RALT == SDL_KMOD_RALT);

/* The shortest time between two changes of full screen. No hand is this
 * fast; a second request inside it is the same keystroke come round again. */
#define LV_FULLSCREEN_GUARD_MS 150u
static Uint64 s_fullScreenLastMs = 0;
static bool   s_fullScreenAny = FALSE;

bool lv_windowCanFullScreen(void) {
    if (g_lv == NULL || g_lv->window == NULL) {
        return FALSE;
    }
    if (uiModeIsSteamDeck() || uiModeIsTablet()) {
        return FALSE;
    }
    return TRUE;
}

bool lv_windowIsFullScreen(void) {
    if (g_lv == NULL || g_lv->window == NULL) {
        return FALSE;
    }
    return (SDL_GetWindowFlags(g_lv->window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void lv_windowToggleFullScreen(void) {
    bool on;
    if (!lv_windowCanFullScreen()) {
        return;
    }
    {
        Uint64 now = SDL_GetTicks();
        if (s_fullScreenAny && now - s_fullScreenLastMs < LV_FULLSCREEN_GUARD_MS) {
            return;
        }
        s_fullScreenAny = TRUE;
        s_fullScreenLastMs = now;
    }
    on = !lv_windowIsFullScreen();
    /* The game's window: the game does it, so its own setting stays right.
     * The viewer's own window, or no game to ask: the same SDL call here. */
    if (g_lv->ownsWindow || !lvGameFullScreenChoose(on)) {
        SDL_SetWindowFullscreen(g_lv->window, on);
        /* The change is not immediate on Wayland and X11, and the frame that
         * follows reads the size of the window. */
        SDL_SyncWindow(g_lv->window);
    }
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
}

/* Alt+Enter. TRUE when the event is that keystroke, a repeat of it included:
 * the caller then gives the event to nothing else. Called before ImGui is
 * given the event, so the Enter is never an Enter in a text box, on a button
 * or in a list, whichever panel has the keyboard. */
static bool lvHostHandleFullScreenKey(const SDL_Event *e) {
    LvFullScreenKey what;
    if (e->type != SDL_EVENT_KEY_DOWN || g_lv == NULL || g_lv->window == NULL ||
        e->key.windowID != SDL_GetWindowID(g_lv->window)) {
        return FALSE;
    }
    what = lvFullScreenKey(e->key.repeat, (unsigned int)e->key.mod,
                           (unsigned int)e->key.scancode);
    if (what == LV_FSKEY_NOT_OURS || !lv_windowCanFullScreen()) {
        return FALSE;
    }
    if (what == LV_FSKEY_TOGGLE) {
        lv_windowToggleFullScreen();
    }
    return TRUE;
}

/* The window has gone into or come out of full screen, by this command or by
 * the system's own (the green button of a Mac). The overview is fitted to the
 * new size by the resize event that comes with it. The game view is not: it
 * has a size of its own that it gave the window. Coming out of full screen it
 * is set up again, so a game view that was started in full screen, where its
 * zoom was cut to fit the display, gets a window of its own size. */
static void lvHostHandleFullScreenChange(const SDL_Event *e) {
    if (g_lv == NULL || g_lv->window == NULL ||
        e->window.windowID != SDL_GetWindowID(g_lv->window)) {
        return;
    }
    if (e->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN && g_lv->gameView) {
        int zoom = lv_drawGameViewGetZoom();
        lv_drawGameViewTeardown();
        lv_drawGameViewSetup(zoom > 0 ? zoom : 3);
        lv_imgui_game_view_init_camera(g_lv);
    }
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
}

/* --------------------------------------------------------------------------
 * lvHostHandleGameViewKey -- Shared game-view input: grave-key game-view
 * toggle, Tab camera cycle, 1-4 zoom, arrow-key pan, ,-. time step. Returns
 * TRUE if the event was consumed (the caller should skip its other handlers).
 * allowToggle turns on the grave-key on/off toggle and allowSeek turns on the
 * ,-. seek. Both hosts, the viewer and the spectator, pass TRUE for both: a
 * live feed can change between the views and step through time as a log
 * file can. */
static bool lvHostHandleGameViewKey(SDL_Event sdlEvent, bool allowToggle,
                                    bool allowSeek) {
    /* While a GIF is made, the view stays as it was when it started: the
     * keys go to its progress window, where Esc cancels it. */
    if (lv_clip_gif_active()) {
        return FALSE;
    }
    /* Game-view-mode toggle. Must run BEFORE ImGui sees the event so
     * ImGui doesn't swallow the backtick keypress. Default zoom 3×.
     * Allowed in both the standalone viewer and the embedded
     * "Watch a Log" flow — game-view borrows whichever window is
     * active and restores it on toggle-off. */
    if (allowToggle && sdlEvent.type == SDL_EVENT_KEY_DOWN &&
        sdlEvent.key.key == SDLK_GRAVE && g_lv->isLoaded) {
        if (!g_lv->gameView) {
            g_lv->savedUseTeamColours = g_lv->useTeamColours;
            g_lv->useTeamColours = FALSE;
            lv_drawGameViewSetup(/* zoom */ 3);
            /* Centre on the camera tank so the first frame after
             * activation isn't a partial viewport into the corner of
             * the map. Subsequent frames use the dead-zone tracker. */
            lv_imgui_game_view_init_camera(g_lv);
        } else {
            lv_drawGameViewTeardown();
            g_lv->useTeamColours = g_lv->savedUseTeamColours;
        }
        g_lv->gameView = !g_lv->gameView;
        lv_drawDirtyScreen();
        g_lv->wantScreenUpdate = TRUE;
        return TRUE;
    }

    /* Tab cycles cameraSlot to the next in-use player. Run BEFORE
     * the ImGui handler so ImGui's widget-focus traversal doesn't
     * eat the keypress while in game view. */
    if (g_lv->gameView && sdlEvent.type == SDL_EVENT_KEY_DOWN &&
        sdlEvent.key.key == SDLK_TAB) {
        BYTE start = g_lv->cameraSlot;
        BYTE found = start;
        BYTE i;
        for (i = 1; i <= MAX_TANKS; i++) {
            BYTE next = (BYTE)((start + i) % MAX_TANKS);
            if (lv_playersIsInUse(next)) {
                found = next;
                break;
            }
        }
        g_lv->cameraSlot = found;
        /* Re-centre on the new camera tank — it may be far outside
         * the previous viewport, and the dead-zone tracker would
         * otherwise scroll one tile per frame to catch up. */
        lv_imgui_game_view_init_camera(g_lv);
        g_lv->wantScreenUpdate = TRUE;
        return TRUE;
    }

    /* Phase E: zoom hotkeys 1/2/3/4 — only meaningful while
     * game view is active. Tear down and re-set up at the
     * new zoom; preserves cameraSlot since that's on
     * LogViewerState, not in game_view.c's statics. */
    if (g_lv->gameView && sdlEvent.type == SDL_EVENT_KEY_DOWN) {
        int newZoom = 0;
        switch (sdlEvent.key.key) {
            case SDLK_1: newZoom = 1; break;
            case SDLK_2: newZoom = 2; break;
            case SDLK_3: newZoom = 3; break;
            case SDLK_4: newZoom = 4; break;
            default: break;
        }
        if (newZoom > 0 && newZoom != lv_drawGameViewGetZoom()) {
            lv_drawGameViewTeardown();
            lv_drawGameViewSetup(newZoom);
            g_lv->wantScreenUpdate = TRUE;
            return TRUE;
        }
    }

    /* Manual arrow-key viewport scroll while game view is active.
     * Pans by kArrowStep unzoomed pixels per key event (SDL fires
     * SDL_EVENT_KEY_DOWN repeatedly while held), going through
     * lv_screenPanToTotalPixels so the pan accumulates at sub-tile
     * granularity into (xOffset,subPxX) instead of snapping a
     * whole tile per repeat. The dead-zone tracker on the next
     * frame may ease the camera back if the pan moved the tank
     * out of the safe zone. */
    if (g_lv->gameView && sdlEvent.type == SDL_EVENT_KEY_DOWN) {
        int dx = 0, dy = 0;
        switch (sdlEvent.key.key) {
            case SDLK_LEFT:  dx = -1; break;
            case SDLK_RIGHT: dx =  1; break;
            case SDLK_UP:    dy = -1; break;
            case SDLK_DOWN:  dy =  1; break;
            default: break;
        }
        if (dx || dy) {
            const int kViewTiles  = 16; /* mirrors GV_SCREEN_TILES */
            const int kTilePx     = 16; /* TILE_SIZE_X */
            const int kArrowStep  = 8;  /* unzoomed px per repeat */
            int totalPxX = (int)g_lv->xOffset * kTilePx + g_lv->subPxX
                         + dx * kArrowStep;
            int totalPxY = (int)g_lv->yOffset * kTilePx + g_lv->subPxY
                         + dy * kArrowStep;
            if (lv_playersIsInUse(g_lv->cameraSlot)) {
                BYTE camMx, camMy, camPx, camPy, camFr;
                bool camBoat;
                lv_playersGetTankDetails(g_lv->cameraSlot,
                    &camMx, &camMy, &camPx, &camPy, &camFr, &camBoat);
                int tankWorldPxX = (int)camMx * kTilePx + (int)camPx;
                int tankWorldPxY = (int)camMy * kTilePx + (int)camPy;
                int viewportPx   = kViewTiles * kTilePx;
                if      (tankWorldPxX <  totalPxX)              totalPxX = tankWorldPxX;
                else if (tankWorldPxX >= totalPxX + viewportPx) totalPxX = tankWorldPxX - viewportPx + 1;
                if      (tankWorldPxY <  totalPxY)              totalPxY = tankWorldPxY;
                else if (tankWorldPxY >= totalPxY + viewportPx) totalPxY = tankWorldPxY - viewportPx + 1;
            }
            lv_screenPanToTotalPixels(totalPxX, totalPxY);
            g_lv->wantScreenUpdate = TRUE;
            return TRUE;
        }
    }

    /* ',' steps one tick back and '.' one tick forward; with Shift ('<' and
     * '>') they jump 10 s, and with Ctrl 1 s. A tick is 20 ms of log, the
     * smallest step the decoder takes. A step leaves playback paused, so the
     * moment stepped to stays on the screen; a jump carries on playing if it
     * was.
     *
     * The keys are matched by scancode, the place of the key on the
     * keyboard, so the two keys right of M on a US keyboard do this on
     * every layout, also where those places give other characters. Shift
     * or Ctrl on either one is a jump; Ctrl and Shift together is the 1 s
     * one. A text box that has the keyboard keeps the key.
     *
     * In game view a jump re-centres the camera, because 10 s can leave the
     * followed tank far outside the viewport and the dead-zone tracker
     * would otherwise scroll one tile per frame to catch up.
     * On the live spectator feed a step parks the view in the past, as the
     * seek bar does. */
    if (allowSeek && sdlEvent.type == SDL_EVENT_KEY_DOWN &&
        g_lv->isLoaded && !lv_imgui_want_text_input() &&
        (sdlEvent.key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI)) == 0 &&
        (sdlEvent.key.scancode == SDL_SCANCODE_COMMA ||
         sdlEvent.key.scancode == SDL_SCANCODE_PERIOD)) {
        const uint32_t kTickMs = 20;
        const uint32_t kJumpMs = 10000;
        const uint32_t kShortJumpMs = 1000;
        bool back = (sdlEvent.key.scancode == SDL_SCANCODE_COMMA);
        bool shortJump = ((sdlEvent.key.mod & SDL_KMOD_CTRL) != 0);
        bool jump = shortJump || ((sdlEvent.key.mod & SDL_KMOD_SHIFT) != 0);
        uint32_t stepMs = shortJump ? kShortJumpMs : (jump ? kJumpMs : kTickMs);
        /* Absolute log time: a 20 ms step is too fine to send through the
         * slider's 0..1 ratio. lv_screenSeekToTimeMs clamps to the log. */
        uint32_t nowMs = lv_screenGetTimeRunning();
        uint32_t targetMs = back ? ((nowMs > stepMs) ? nowMs - stepMs : 0)
                                 : nowMs + stepMs;
        unsigned char wasPlaying = g_lv->playIsPlaying;
        if (wasPlaying) lv_windowPause();
        lv_clientMutexWaitFor();
        lv_drawDirtyScreen();
        lv_screenSeekToTimeMs(targetMs);
        lv_drawDirtyScreen();
        lv_clientMutexRelease();
        if (jump && g_lv->gameView) lv_imgui_game_view_init_camera(g_lv);
        lv_windowNeedRedraw();
        if (wasPlaying && jump) lv_windowPlay();
        g_lv->wantScreenUpdate = TRUE;
        return TRUE;
    }

    return FALSE;
}

/* --------------------------------------------------------------------------
 * lvHostHandleResize -- Shared SDL_EVENT_WINDOW_RESIZED handler.
 *
 * Snaps the window to the nearest zoom-effective tile boundary, updates the
 * screen tile counts and clamps the scroll offset, then recreates the
 * render-target texture (without which mouse-to-tile coordinates drift) and
 * marks the screen dirty. Game view drives a fixed window size, so a resize
 * there is ignored. Shared by logViewerRun and spectatorRun so the live
 * spectator reflows its overview exactly like the standalone viewer.
 * -------------------------------------------------------------------------- */
static void lvHostHandleResize(const SDL_Event *e) {
    if (g_lv->gameView) {
        return;
    }
    int w = e->window.data1;
    int h = e->window.data2;
    int menuH = (int)lv_imgui_get_menu_bar_height();
    float zoom = lv_drawGetZoomLevel();
    if (zoom <= 0.0f) zoom = 1.0f;

    /* Snap to the nearest zoom-effective tile boundary so the blit fills the
     * window cleanly at the current zoom. */
    float tilePxX = (float)TILE_SIZE_X * zoom;
    float tilePxY = (float)TILE_SIZE_Y * zoom;
    /* The sidebars that show take the edges of the window; the map is
     * fitted to what is left. All zero whenever every sidebar is off, which
     * leaves every sum below as it was. */
    /* A sidebar whose unused part is clear takes nothing from the map. */
    LvDockMargins dock = { 0, 0, 0, 0 };
    lv_imgui_dock_map_margins(&dock.left, &dock.top, &dock.right, &dock.bottom);
    int reservedX = dock.left + dock.right;
    int reservedY = dock.top + dock.bottom;
    int newTilesX = lvFitTilesNearest(w - reservedX, tilePxX);
    int newTilesY = (int)((((float)(h - menuH - reservedY)) + tilePxY * 0.5f) / tilePxY);
    if (newTilesY < 1) newTilesY = 1;
    if (newTilesY > 255) newTilesY = 255;

    /* A maximized or fullscreen window can't be resized: SDL_SetWindowSize is
     * a no-op there, so snapping would leave the window at its full size while
     * the tile counts and render target below shrink to the snapped size. That
     * window/render-target/tile mismatch desyncs the cursor-anchored zoom
     * (lv_drawApplyZoomStep clamps the anchor against the render target), which
     * is why wheel/controller zoom wedges once the window is maximized. In that
     * state, fit the tile counts to the actual window (the event's data1/data2
     * are the real maximized dimensions) and leave the window alone. A normal
     * windowed resize still snaps to a clean tile boundary. */
    /* A window -size fixed is not snapped either: it must stay the size
     * asked for, so it is fitted as a maximized one is. */
    SDL_WindowFlags wflags = SDL_GetWindowFlags(g_lv->window);
    if (!lvHostFixedWindowSize() && !(wflags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        int snappedW = (int)(newTilesX * tilePxX + 0.5f);
        int snappedH = (int)(newTilesY * tilePxY + 0.5f);
        SDL_SetWindowSize(g_lv->window, snappedW + reservedX,
                          snappedH + menuH + reservedY);
    } else {
        /* The window stays as it is, so the map cannot be snapped to end at
         * a sidebar. Rounding to the nearest square could stop it short and
         * leave a black strip beside the sidebar; cover the gap instead, with
         * the last square running a little way under the sidebar's edge. */
        if (reservedX > 0) newTilesX = lvFitTilesCover(w - reservedX, tilePxX);
        if (reservedY > 0) newTilesY = lvFitTilesCover(h - menuH - reservedY, tilePxY);
    }
    lvHostDockNoteFitted(&dock);
    lv_screenSetSizeX((BYTE)newTilesX);
    lv_screenSetSizeY((BYTE)newTilesY);
    /* The size setters keep the offset inside the map for the new grid. The
     * sub-pixel pan is reset here so the resized viewport snaps cleanly to
     * tile boundaries: a window resize carries no in-flight drag. */
    lv_screenSetSubOffset(0, 0);
    lv_drawResizeRenderTarget();
    lv_drawDirtyScreen();
    g_lv->wantScreenUpdate = TRUE;
}

/* --------------------------------------------------------------------------
 * lvHostRenderFrame -- Shared per-frame scene + ImGui render dispatch.
 *
 * Renders the current mode: the block-grid overview when !gameView, the
 * followed game view when gameView (with the cameraSlot setSelf wrap so tank
 * sprite colours track the camera), then runs the ImGui pass for that mode and
 * presents. Pure extraction of logViewerRun's frame body, shared with
 * spectatorRun.
 *
 * overlay, when non-NULL, is drawn centred on the map just before the ImGui
 * render (the live spectator's "connection lost" message); the standalone
 * viewer passes NULL.
 *
 * A live feed (lv_screenSpecIsLiveMode, which only spectatorRun sets) draws
 * the same overview as a log file: the menu bar, the sidebars and every
 * panel, each as its own Windows menu option says (lvPanelSetDesktop). The
 * differences:
 *   - the menu bar has only the items that cannot end the feed
 *     (imgui_main_menu.cpp);
 *   - the Comments panel is not drawn, because a feed has no WinBolo.net log;
 *   - the badge with the phase of the game is drawn in the corner of the map.
 * In the game view a live feed also keeps the Controls panel, so the feed
 * can be moved through in time there too. */
static void lvHostRenderFrame(const char *overlay) {
    lvHostDockSync();
    lvHostFirstView();
    lvJumpAnimFrame();

    /* Clear the frame */
    SDL_SetRenderDrawColor(g_lv->renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_lv->renderer);

    if (g_lv->isLoaded == FALSE) {
        /* No log, or one still opening: the window stays black under the
           panels and the progress window. Draw to the window itself, not to
           a render target the last log left current. */
        SDL_SetRenderTarget(g_lv->renderer, NULL);
        SDL_RenderClear(g_lv->renderer);
    } else {
        /* In game-view mode, recentre the camera on the spectated tank
         * BEFORE lv_screenUpdate runs so the tile renderer reads the
         * fresh offsets this frame. */
        if (g_lv->gameView) {
            lv_imgui_game_view_update_camera(g_lv);
        }
        /* Game view renders directly to the framebuffer (no
         * textureTarget caching) so the lv_drawBlitGameTexture
         * fallback would just blit a stale logviewer frame on top of
         * a freshly-rendered game-view frame. Force the full update
         * path while game view is active. */
        /* A perspective whose player has left the game goes back to all
         * teams. Asked here, before the game view below borrows "self". */
        if (lv_playersHasPerspective() == TRUE) {
            lv_clientMutexWaitFor();
            if (lv_screenCheckPerspective() == TRUE) {
                lv_drawDirtyScreen();
            }
            lv_clientMutexRelease();
        }
        if (g_lv->wantScreenUpdate == TRUE || g_lv->gameView) {
            BYTE savedSelf = 0;
            bool didSetSelf = FALSE;
            if (g_lv->gameView) {
                /* Set self to the spectated tank BEFORE lv_screenUpdate
                 * builds screenTanks. Tank sprite frame indices
                 * (TANK_SELF_* / GOOD_* / EVIL_*) are baked from
                 * lv_playersGetSelf at update time — without this, map
                 * sprite colours would lock to whatever self was when the
                 * log started and wouldn't follow Tab. */
                savedSelf = lv_playersGetSelf();
                lv_playersSetSelf(g_lv->cameraSlot);
                didSetSelf = TRUE;
            }
            lv_clientMutexWaitFor();
            /* Overview repaint is incremental: the per-tile dirty cache
             * (lv_drawLast) combined with each sprite's lv_drawMarkRedraw
             * (tanks/shells/LGMs/labels) erases moving overlays, so only
             * changed tiles repaint into the persistent render target.
             * Forcing a full-screen dirty every frame instead repaints every
             * visible tile — at a zoomed-out 255x255 viewport (e.g. fullscreen
             * on a 4K display) that is ~65k tile blits per frame and stalls the
             * render. Pan/zoom/resize already dirty the whole screen on demand.
             * Game view renders directly (no render-target cache) and needs the
             * full update each frame. */
            if (g_lv->gameView) {
                lv_drawDirtyScreen();
            }
            lv_screenUpdate(redraw);
            lv_clientMutexRelease();
            g_lv->wantScreenUpdate = FALSE;
            if (didSetSelf) {
                lv_playersSetSelf(savedSelf);
            }
        } else {
            lv_drawBlitGameTexture();
        }
    }

    /* This frame's share of a GIF being made (the GIF button of recording mode): the log
     * moves on and the map is drawn into its render target for each frame of
     * the GIF. After the map above and before the ImGui frame. */
    lv_clip_gif_capture_step();

    /* Render ImGui UI */
    lv_imgui_context_newframe();
#ifdef __APPLE__
    /* Marshal in-window menu state into the native NSMenu once per
     * frame. Cheap walk over cached NSMenuItem pointers; checkmarks
     * and enable states mirror the ImGui menu's predicates. */
    {
        struct LvMenuState lvms;
        memset(&lvms, 0, sizeof(lvms));
        lvms.isLoaded         = g_lv->isLoaded ? true : false;
        lvms.playIsPlaying    = g_lv->playIsPlaying ? true : false;
        lvms.modeInformation  = lv_imgui_get_mode_information() ? true : false;
        lvms.useTeamColours   = g_lv->useTeamColours ? true : false;
        lvms.gameViewActive   = g_lv->gameView ? true : false;
        lvms.tankCentred      = lv_imgui_get_tank_centred() ? true : false;
        lvms.hideLobby        = lv_screenGetHideLobby() ? true : false;
        lvms.showRegions      = lv_g_show_regions;
        lvms.hasRegions       = lv_imgui_has_regions() ? true : false;
        lvms.soundEffects     = g_lv->isSoundsPlaying ? true : false;
        lvms.soundVolume      = g_lv->soundVolume;
        lvms.dnsLookups       = lv_imgui_get_dns_lookups() ? true : false;
        lvms.showControls     = lv_g_show_controls_window;
        lvms.showEvents       = lv_g_show_events_window;
        lvms.showGameInfo     = lv_g_show_game_info_window;
        lvms.showItemInfo     = lv_g_show_item_info_window;
        lvms.showComments     = lv_g_show_comments_window;
        lvms.showScenarioPanel = lv_g_show_scenario_panel_window;
        lvms.showHud          = lv_g_show_hud_window;
        lvms.showOverview     = lv_g_show_overview_window;
        lvms.showTank         = lv_g_show_tank_window;
        lvms.tankBars         = lv_drawGetTankBars() ? true : false;
        lvms.builderStatus    = lv_drawGetBuilderStatus() ? true : false;
        lvms.pillNumbers      = lv_drawGetPillNumbers() ? true : false;
        lvms.dockEnabled      = lv_imgui_dock_get_enabled() ? true : false;
        lvms.dockLeft         = lv_imgui_dock_get_side_enabled(LV_DOCK_LEFT) ? true : false;
        lvms.dockTop          = lv_imgui_dock_get_side_enabled(LV_DOCK_TOP) ? true : false;
        lvms.dockBottom       = lv_imgui_dock_get_side_enabled(LV_DOCK_BOTTOM) ? true : false;
        lvms.zoomStepIndex    = lv_drawGetZoomStepIndex();
        lvms.zoomStepCount    = lv_drawGetZoomStepCount();
        lvms.fromMainMenu     = g_lv->fromMainMenu ? true : false;
        lvms.fullScreen       = lv_windowIsFullScreen() ? true : false;
        lvms.canFullScreen    = lv_windowCanFullScreen() ? true : false;
        lvms.live             = lv_screenSpecIsLiveMode() ? true : false;
        lvms.gifAvailable     = lv_clip_gif_available() ? true : false;
        lvms.recordMode       = lv_clip_gif_mode_on() ? true : false;
        lvms.canToggleRecordMode = lv_clip_gif_can_toggle_mode() ? true : false;
        lvms.showClips        = lv_clip_gif_gallery_is_open() ? true : false;
        lv_mac_menubar_refresh(&lvms);
    }
#endif
    if (g_lv->gameView) {
        lv_imgui_render_game_menu_bar(g_lv);
        lv_imgui_render_game_view(g_lv);
        /* The camera tank's panel, as its player saw it. */
        lv_imgui_scenario_panel_window(g_lv->isLoaded ? true : false);
        lv_imgui_scenario_status(g_lv->isLoaded ? true : false);
        {
            /* Whose screen the map shows, while the lag view is on. */
            int winW = 0, winH = 0;
            SDL_GetWindowSize(g_lv->window, &winW, &winH);
            lv_imgui_lag_banner_at(0.0f, lv_imgui_get_menu_bar_height(), (float)winW);
        }
        /* A live feed keeps the Controls panel in the game view, so the feed
         * can be moved through in time there; the viewer's game view has
         * none. The option is put back after, so the forced value is never
         * saved. */
        if (lv_screenSpecIsLiveMode()) {
            bool save_ctrl = lv_g_show_controls_window;
            lv_g_show_controls_window = true;
            lv_imgui_controls_window();
            lv_g_show_controls_window = save_ctrl;
            lv_imgui_spectator_badge(g_lv->gamePhase);
        } else {
            /* No Controls panel for the GIF window to go over; this also
             * gives it View > Reset Window Positions. */
            lv_clip_gif_set_anchor(false, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        lv_g_reset_window_positions = false;
        if (overlay != NULL) {
            lv_imgui_center_message(overlay);
        }
        lv_imgui_zoom_label();
        lvHostDrawLoadProgress();
        /* The Clips window stays open in the game view. */
        lv_clip_gif_render();
        lv_imgui_dialogs_render();
        lv_imgui_context_render();
    } else {
        bool compact = (uiModeIsTablet() || uiModeIsSteamDeck());
        const unsigned panels = lvPanelSetDesktop(lv_screenSpecIsLiveMode() ? 1 : 0);
        if (compact) {
            /* Start opens the popup menu — replaces the desktop top
             * menu bar in tablet/Deck compact mode. */
            lvMenuPollOpenInput();
            /* Each panel _window() early-outs on its lv_g_show_* flag
             * (desktop-side visibility prefs).  In compact mode there
             * is no menu to toggle those, so save/force/restore around
             * the calls — never persisting any change. */
            bool save_ctrl  = lv_g_show_controls_window;
            bool save_evt   = lv_g_show_events_window;
            bool save_gi    = lv_g_show_game_info_window;
            bool save_ii    = lv_g_show_item_info_window;
            bool save_cmt   = lv_g_show_comments_window;
            /* A live feed has no Comments panel (lvPanelSetDesktop): the
             * Events panel is up in its place. */
            LvCompactPanel active = lvCompactGetActivePanel();
            if (active == LV_PANEL_COMMENTS && !(panels & LV_PANEL_SET_COMMENTS)) {
                active = LV_PANEL_EVENTS;
            }
            lv_g_show_controls_window  = true;
            lv_g_show_events_window    = (active == LV_PANEL_EVENTS);
            lv_g_show_game_info_window = (active == LV_PANEL_GAME_INFO);
            lv_g_show_item_info_window = (active == LV_PANEL_ITEM_INFO);
            lv_g_show_comments_window  = (active == LV_PANEL_COMMENTS);

            /* Toolbar (controls window) is always visible in compact;
             * it IS the toolbar (playback + speed + seek). */
            lv_imgui_controls_window();
            /* Single content panel, switched via the popup menu. */
            switch (active) {
                case LV_PANEL_GAME_INFO: lv_imgui_game_info_window(); break;
                case LV_PANEL_ITEM_INFO: lv_imgui_item_info_window(); break;
                case LV_PANEL_COMMENTS:  lv_imgui_comments_window();  break;
                case LV_PANEL_EVENTS:
                default:                 lv_imgui_events_window();    break;
            }
            /* The panel square beside whichever panel is up: a scenario's
             * timer is part of watching the round, not a panel to switch
             * to, and there is no menu here to bring it back if closed. */
            {
                bool save_scn = lv_g_show_scenario_panel_window;
                lv_g_show_scenario_panel_window = true;
                lv_imgui_scenario_panel_window(g_lv->isLoaded ? true : false);
                lv_imgui_scenario_status(g_lv->isLoaded ? true : false);
                lv_g_show_scenario_panel_window = save_scn;
            }

            lv_g_show_controls_window  = save_ctrl;
            lv_g_show_events_window    = save_evt;
            lv_g_show_game_info_window = save_gi;
            lv_g_show_item_info_window = save_ii;
            lv_g_show_comments_window  = save_cmt;
        } else {
            lv_imgui_main_menu_bar();
            lv_imgui_cache_menu_bar_height();
            /* Before the panels: a panel docked in it has to find the dock
             * space already submitted this frame, or ImGui undocks it. */
            lv_imgui_dock_host();
            lv_imgui_controls_window();
            /* Desktop layout only: the game view has the game's own status
             * grids, and the compact layout has no room for another panel. */
            lv_imgui_hud_window();
            lv_imgui_overview_window();
            lv_imgui_tank_window();
            lv_imgui_game_info_window();
            lv_imgui_events_window();
            lv_imgui_item_info_window();
            if (panels & LV_PANEL_SET_COMMENTS) {
                lv_imgui_comments_window();
            }
            lv_imgui_scenario_panel_window(g_lv->isLoaded ? true : false);
            lv_imgui_scenario_status(g_lv->isLoaded ? true : false);
        }
        /* The live feed's badge and message go on the map, not under a
         * sidebar or the menu bar. */
        {
            /* Whose screen the map shows, while the lag view is on. */
            float mapX, mapY, mapW, mapH;
            lvHostMapArea(&mapX, &mapY, &mapW, &mapH);
            lv_imgui_lag_banner_at(mapX, mapY, mapW);
        }
        if ((panels & LV_PANEL_SET_SPEC_BADGE) || overlay != NULL) {
            float mapX, mapY, mapW, mapH;
            lvHostMapArea(&mapX, &mapY, &mapW, &mapH);
            if (panels & LV_PANEL_SET_SPEC_BADGE) {
                lv_imgui_spectator_badge_at(g_lv->gamePhase, mapX + mapW, mapY);
            }
            if (overlay != NULL) {
                lv_imgui_center_message_at(overlay, mapX, mapY, mapW, mapH);
            }
        }
        lv_g_reset_window_positions = false;
        /* Popup stacks over panels, under blocking modal dialogs. */
        if (compact) {
            lvMenuRender(g_lv);
        }
        lv_imgui_zoom_label();
        lv_imgui_game_viewport_draw_marks();
        lv_imgui_game_viewport_hover();
        lvHostDrawLoadProgress();
        /* Recording mode's crop frame, a GIF's progress and the Clips
         * window. */
        lv_clip_gif_render();
        lv_imgui_dialogs_render();
        lv_imgui_context_render();

        lv_imgui_game_viewport_process_input();
    }

    SDL_RenderPresent(g_lv->renderer);
}

/* --------------------------------------------------------------------------
 * logViewerRun -- Core viewer loop.
 *
 * Takes ownership of the event loop until the user exits.
 * Window and renderer are borrowed, not owned.
 * -------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------
 * lvHostAutosave -- Write the preferences soon after the layout changes.
 *
 * Called once a frame by the standalone viewer. The layout (sidebars, panels,
 * the window's own place) is written when the viewer closes, but a viewer
 * that is killed never gets there, so it is also written a moment after any
 * change to it: the most that can be lost is the last second or two.
 * Unchanged values cost nothing; the preferences store only writes its file
 * when a value differs.
 *
 * Not in game view: it runs at a fixed size of its own, which must not be
 * saved as the viewer's.
 * -------------------------------------------------------------------------- */
static void lvHostAutosave(void) {
    static int    seen[6] = { 0, 0, 0, 0, 0, 0 };
    static bool   haveSeen = FALSE;
    static bool   pending = FALSE;
    static Uint64 pendingAt = 0;
    int now[6] = { 0, 0, 0, 0, 0, 0 };
    bool due;

    if (g_lv == NULL || g_lv->gameView) {
        return;
    }
    due = lv_imgui_dock_autosave_due() != 0;

    if (g_lv->ownsWindow) {
        SDL_GetWindowPosition(g_lv->window, &now[0], &now[1]);
        SDL_GetWindowSize(g_lv->window, &now[2], &now[3]);
        now[4] = (SDL_GetWindowFlags(g_lv->window) & SDL_WINDOW_MAXIMIZED) ? 1 : 0;
        now[5] = (SDL_GetWindowFlags(g_lv->window) & SDL_WINDOW_FULLSCREEN) ? 1 : 0;
        if (!haveSeen) {
            memcpy(seen, now, sizeof(seen));
            haveSeen = TRUE;
        } else if (memcmp(seen, now, sizeof(seen)) != 0) {
            /* Still moving: wait until it has been still for a second. */
            memcpy(seen, now, sizeof(seen));
            pending = TRUE;
            pendingAt = SDL_GetTicks();
        } else if (pending && SDL_GetTicks() - pendingAt >= 1000) {
            pending = FALSE;
            due = TRUE;
        }
    }
    if (due) {
        savePreferences();
    }
}

/* The viewer ends its loop on SDL_EVENT_QUIT, and both "Return to main menu"
 * and Quit raise one — the first means go back to WinBolo, the second means
 * the application is finished.  Quit says so here on its way past, and the
 * caller reads it with logViewerAppQuitRequested() once the run returns. */
static bool s_appQuitRequested = FALSE;

void logViewerRequestAppQuit(void) {
    SDL_Event quitEvent;
    s_appQuitRequested = TRUE;
    SDL_zero(quitEvent);
    quitEvent.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quitEvent);
}

bool logViewerAppQuitRequested(void) {
    return s_appQuitRequested;
}

void logViewerRun(SDL_Window *window, SDL_Renderer *renderer,
                  const char *logPath, bool fromMainMenu) {
    /* A quit belongs to the run that saw it — never to the next one. */
    s_appQuitRequested = FALSE;

    /* Bring up the decoder, platform layer, draw/ImGui/sound and preferences,
     * and size the screen to the window (shared with spectatorRun). */
    if (lvHostSetup(window, renderer, fromMainMenu) == FALSE) {
        return;
    }
    /* The sidebars, and the defaults of the options a new user has not set.
     * The spectator (spectatorRun) does the same, so the two share one saved
     * layout. */
    lv_imgui_dock_set_standalone(1);
    lv_imgui_main_menu_standalone_defaults();
    lvHostFirstViewSetHost(TRUE);
    s_lvJump.active = FALSE;
    s_lvJumpHost = TRUE;
    lvHostRestoreWindowPlace();
    lvHostWarnFixedWindowSize();

    /* Open file from command line / caller if provided. Like a pending
     * memory buffer from logViewerRunFromMemory, it opens on the first frame. */
    if (logPath != NULL && strlen(logPath) > 0) {
        lv_windowOpenFile((char *)logPath);
    }

    /* -----------------------------------------------------------------------
     * Main event loop
     * ----------------------------------------------------------------------- */
    while (!g_lv->quit) {
        SDL_Event sdlEvent;

        /* A log asked for since the last frame starts opening here. */
        lvHostLoadBegin();

        /* Process SDL events */
        while (SDL_PollEvent(&sdlEvent)) {
            /* Alt+Enter: full screen. First, so nothing else has the key. */
            if (lvHostHandleFullScreenKey(&sdlEvent)) {
                continue;
            }
            if (sdlEvent.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
                sdlEvent.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
                lvHostHandleFullScreenChange(&sdlEvent);
            }
            /* Game-view input (grave toggle / Tab cycle / 1-4 zoom / arrow pan
             * / ,-. seek), shared with spectatorRun. Both hosts allow the
             * toggle and the seek. Not while a log is opening: a seek would
             * move the decoder the load is reading through. */
            if (!s_loading &&
                lvHostHandleGameViewKey(sdlEvent, /* allowToggle */ TRUE,
                                        /* allowSeek */ TRUE)) {
                continue;
            }

            lv_imgui_context_handle_event(&sdlEvent);

            if (sdlEvent.type == SDL_EVENT_RENDER_TARGETS_RESET ||
                sdlEvent.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                lv_drawDirtyScreen();
            }
            if (sdlEvent.type == SDL_EVENT_WINDOW_EXPOSED ||
                sdlEvent.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                lv_drawDirtyScreen();
                g_lv->wantScreenUpdate = TRUE;
            }
            if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED) {
                lvHostHandleResize(&sdlEvent);
            }
            if (sdlEvent.type == SDL_EVENT_MOUSE_WHEEL) {
                /* Stepped zoom anchored on the cursor. Skip when ImGui is
                 * using the wheel (cursor over a panel) so panel scrolling
                 * still works. The wheel event carries the mouse position
                 * in window coordinates (mouse_x / mouse_y). Suppressed in
                 * game-view mode — the trailer window size is fixed and
                 * mouse-wheel zoom would invalidate the saved teardown
                 * dimensions. */
                if (!lv_imgui_want_capture_mouse() && !g_lv->gameView &&
                    !lv_clip_gif_active()) {
                    int mx = (int)sdlEvent.wheel.mouse_x;
                    int my = (int)sdlEvent.wheel.mouse_y;
                    if (sdlEvent.wheel.y > 0.0f) {
                        lv_drawZoomIn(mx, my);
                    } else if (sdlEvent.wheel.y < 0.0f) {
                        lv_drawZoomOut(mx, my);
                    }
                }
            }
            /* Embedded, this is WinBolo's window the viewer is drawing into;
             * a close request meant for anything else is not ours. */
            if (sdlEvent.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                sdlEvent.window.windowID == SDL_GetWindowID(g_lv->window)) {
                logViewerRequestAppQuit();
            }
            if (sdlEvent.type == SDL_EVENT_QUIT) {
                g_lv->quit = TRUE;
                break;
            }
        }

        /* Trackpad pinch-to-zoom (macOS) — anchor on current mouse position.
         * Suppressed in game-view mode; trailer window is fixed-size. */
        if (!g_lv->gameView && !lv_clip_gif_active()) {
            float pinch = macOSPinchZoomConsume();
            if (pinch != 0.0f) {
                static float pinchAccum = 0.0f;
                pinchAccum += pinch;
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                while (pinchAccum > 0.15f) {
                    lv_drawZoomIn((int)mx, (int)my);
                    pinchAccum -= 0.15f;
                }
                while (pinchAccum < -0.15f) {
                    lv_drawZoomOut((int)mx, (int)my);
                    pinchAccum += 0.15f;
                }
            }
        }

        /* This frame's share of opening a log. */
        lvHostLoadSteps();

        /* Render the current mode (block-grid / game view) and present. */
        lvHostRenderFrame(NULL);

        /* Keep the saved layout no more than a moment behind the screen. */
        lvHostAutosave();

        /* The standalone viewer's preference sets only mark the document
         * (merge-on-save, main.c). Whatever was set this frame by code that
         * does not save for itself (the Events filter, the shared network
         * code) goes to the file here, in one write. Nothing was set: no
         * write. A write that fails waits two seconds before the next try.
         * Inside the game this does nothing; the game writes. */
        lv_platform_config_save();
    }

    /* -----------------------------------------------------------------------
     * Shutdown
     * ----------------------------------------------------------------------- */
    /* If the user quit while game view was active, tear it down first so the
     * screen size restored to LogViewerState matches the pre-game-view value
     * — otherwise savePreferences would persist the game-view's 16×16 size
     * as the default for next launch. */
    if (g_lv->gameView) {
        lv_drawGameViewTeardown();
        g_lv->useTeamColours = g_lv->savedUseTeamColours;
        g_lv->gameView = FALSE;
    }
    savePreferences();
    lvHostTeardown();
}

/* --------------------------------------------------------------------------
 * logViewerRunFromMemory -- Load a log from an in-memory zip buffer.
 *
 * Identical to logViewerRun but loads from memory instead of file.
 * Takes ownership of zipData (freed when log viewer closes).
 * -------------------------------------------------------------------------- */
void logViewerRunFromMemory(SDL_Window *window, SDL_Renderer *renderer,
                            uint8_t *zipData, size_t zipLen, bool fromMainMenu) {
    /* Use static pending vars that logViewerRun checks after init */
    s_pendingZipData = zipData;
    s_pendingZipLen  = zipLen;
    logViewerRun(window, renderer, NULL, fromMainMenu);
}

/* Set the borrowed window's title for the live session: host:port, the map name
 * decoded from the current seed, and a phase tag ([Lobby] / [Live] / [Game Over],
 * omitted for an unknown phase). Re-applied on every (re)seed so the map name and
 * phase refresh when the round or phase changes. The caller restores the app
 * title on return. */
static void lvSpecApplyTitle(const char *serverHost, uint16_t serverPort) {
    char specTitle[176];
    char phaseTag[64];
    /* Localized phase tag: the bracketed badge wraps the already-localized
       phase string (UNKNOWN keeps an empty tag). */
    switch (g_lv->gamePhase) {
        case SPEC_PHASE_LOBBY:
        case SPEC_PHASE_COUNTDOWN:
            snprintf(phaseTag, sizeof(phaseTag), " [%s]",
                     langGetText(STR_LV_SPEC_PHASE_LOBBY));
            break;
        case SPEC_PHASE_RUNNING:
            snprintf(phaseTag, sizeof(phaseTag), " [%s]",
                     langGetText(STR_LV_SPEC_PHASE_LIVE));
            break;
        case SPEC_PHASE_GAMEOVER:
            snprintf(phaseTag, sizeof(phaseTag), " [%s]",
                     langGetText(STR_LV_SPEC_PHASE_GAMEOVER));
            break;
        default:
            phaseTag[0] = '\0';
            break;
    }
    if (g_lv->mapName[0] != '\0') {
        /* phaseTag is the localized " [Lobby]"/" [Live]"/... tag carried in
           {string3}; the host and map flow through {string1}/{string2}. */
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%s", serverHost ? serverHost : "");
        args.number = (int)serverPort;
        snprintf(args.string2, sizeof(args.string2), "%s", g_lv->mapName);
        snprintf(args.string3, sizeof(args.string3), "%s", phaseTag);
        snprintf(specTitle, sizeof(specTitle), "%s",
                 langGetTextFmt(STR_LV_SPEC_WINDOW_TITLE_FMT, &args));
    } else {
        MessageArgs args = {0};
        snprintf(args.string1, sizeof(args.string1), "%s", serverHost ? serverHost : "");
        args.number = (int)serverPort;
        snprintf(args.string3, sizeof(args.string3), "%s", phaseTag);
        snprintf(specTitle, sizeof(specTitle), "%s",
                 langGetTextFmt(STR_LV_SPEC_WINDOW_TITLE_NOMAP_FMT, &args));
    }
    SDL_SetWindowTitle(g_lv->window, specTitle);
}

/* --------------------------------------------------------------------------
 * spectatorRun -- Modal host for the live delayed spectator feed.
 *
 * Borrows the caller's window/renderer (never NULL here) and runs a self-driven
 * decoder loop: it pumps the spectator transport, drains the captured seed and
 * forward records through the decoder, forces the game view on and renders the
 * delayed feed each frame, then tears down. Unlike logViewerRun it never calls
 * lv_windowPlay — there is no log file and no background replay timer; this loop
 * advances the decoder itself from the drained records.
 *
 * cs is the bolo-world ClientSim as an opaque handle: the transport pump and the
 * seed/record drain go through the dependency-free spectator_drain.h seam so this
 * logviewer-world TU never includes client_sim.h (its screenObj conflicts with
 * backend.h). The ClientSim's lifetime is the caller's — spectatorRun does not
 * disconnect or free it.
 * -------------------------------------------------------------------------- */
bool spectatorRun(SDL_Window *window, SDL_Renderer *renderer, void *cs,
                  const char *serverHost, uint16_t serverPort) {
    /* True when the loop exits because the server put the spectator back into
     * live-lobby mode (its delayed game drained to the lobby); false when the
     * user left or the feed never loaded. The caller's dual-mode loop re-enters
     * the live lobby on true and exits on false. */
    bool liveResumed = false;

    /* A quit belongs to the run that saw it — never to the next one.  Same
     * reset logViewerRun does: this loop can raise the flag too, and the
     * caller reads it once we return. */
    s_appQuitRequested = FALSE;

    if (lvHostSetup(window, renderer, /* fromMainMenu */ TRUE) == FALSE) {
        return false;
    }
    /* The same as the viewer (logViewerRun): the sidebars, the defaults of
     * the options a new user has not set, the first view on the land, and
     * the animation of Rewind and Fast Forward. The two share one saved
     * layout. The window is the game's, so its place is not restored. */
    lv_imgui_dock_set_standalone(1);
    lv_imgui_main_menu_standalone_defaults();
    lvHostFirstViewSetHost(TRUE);
    s_lvJump.active = FALSE;
    s_lvJumpHost = TRUE;

    /* Stalled-feed watchdog. A spectator sends no input, so the transport's
     * normal liveness timeout never fires — the host times the wait itself.
     * lastProgressMs is bumped on any seam progress (a fresh countdown value, a
     * drained record); kSpecStallMs of dead air surfaces a recoverable
     * "connection lost" the user can exit from. */
    const uint32_t kSpecStallMs = 8000;   /* ~8s with no feed progress */
    uint32_t lastProgressMs = SDL_GetTicks();
    uint32_t lastCountdown  = 0;
    bool     haveCountdown  = false;
    bool     connectionLost = false;

    /* Await the captured seed: pump the transport and service events until the
     * seed is ready or the user quits. Show a "spectating begins in N" overlay
     * once the cold-start countdown arrives, a "connecting" placeholder before
     * it, or "connection lost" if the feed stalls. When the feed stalls the seed
     * never arrives, so this loop keeps showing the message until the user
     * quits. */
    while (!g_lv->quit && !specDrainSeedReady(cs)) {
        SDL_Event sdlEvent;
        uint32_t remaining = 0;
        char overlay[64];

        specDrainPump(cs);
        while (SDL_PollEvent(&sdlEvent)) {
            /* Alt+Enter: full screen, while the feed is waited for too. */
            if (lvHostHandleFullScreenKey(&sdlEvent)) {
                continue;
            }
            lv_imgui_context_handle_event(&sdlEvent);
            /* Esc is the back affordance: leave the spectator view (including
             * a stalled "connection lost" overlay) and return to the caller. */
            /* Embedded, this is WinBolo's window the viewer is drawing into;
             * a close request meant for anything else is not ours. */
            if (sdlEvent.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                sdlEvent.window.windowID == SDL_GetWindowID(g_lv->window)) {
                logViewerRequestAppQuit();
            }
            if (sdlEvent.type == SDL_EVENT_QUIT ||
                (sdlEvent.type == SDL_EVENT_KEY_DOWN &&
                 sdlEvent.key.key == SDLK_ESCAPE)) {
                g_lv->quit = TRUE;
                break;
            }
        }

        /* A changed countdown value (or its first arrival) is feed progress. */
        if (specDrainCountdown(cs, &remaining) &&
            (!haveCountdown || remaining != lastCountdown)) {
            haveCountdown  = true;
            lastCountdown  = remaining;
            lastProgressMs = SDL_GetTicks();
        }
        if (!connectionLost && (SDL_GetTicks() - lastProgressMs) > kSpecStallMs) {
            connectionLost = true;
        }

        SDL_SetRenderDrawColor(g_lv->renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_lv->renderer);
        lv_imgui_context_newframe();
        if (connectionLost) {
            lv_imgui_center_message(langGetText(STR_LV_SPEC_CONNECTION_LOST));
        } else if (haveCountdown) {
            /* ~50 ticks/sec; round up so the last second shows "1", not "0". */
            uint32_t secs = (lastCountdown + 49) / 50;
            if (secs == 0) {
                lv_imgui_center_message(langGetText(STR_LV_SPEC_BEGINS_NOW));
            } else {
                MessageArgs args = {0};
                args.number = (int)secs;
                snprintf(overlay, sizeof(overlay), "%s",
                         langGetTextFmt(STR_LV_SPEC_BEGINS_IN_FMT, &args));
                lv_imgui_center_message(overlay);
            }
        } else {
            lv_imgui_center_message(langGetText(STR_LV_SPEC_CONNECTING));
        }
        lv_imgui_context_render();
        SDL_RenderPresent(g_lv->renderer);
    }

    /* Seed the decoder and open in the block-grid overview. The seed load is
     * guarded: a failed/absent seed leaves isLoaded FALSE and the steady loop
     * below is skipped — spectatorRun falls through to a clean teardown.
     * gameView stays FALSE; the grave key drives the on-demand game-view
     * setup/teardown once the spectator is running. */
    if (!g_lv->quit) {
        uint8_t *seed = NULL;
        uint32_t seedLen = 0;
        if (specDrainTakeSeed(cs, &seed, &seedLen)) {
            lvSpecFeedSeed(seed, seedLen);
            free(seed);
        }

        if (g_lv->isLoaded) {
            lvSpecApplyTitle(serverHost, serverPort);

            lv_drawDirtyScreen();
            g_lv->wantScreenUpdate = TRUE;
            /* A delivered seed means the feed is alive: clear any await-phase
             * stall and restart the watchdog for the record stream. */
            connectionLost = false;
            lastProgressMs = SDL_GetTicks();
            /* Enter live-DVR mode: the stream pump now appends only and this loop
             * drives the advance, so the scrubber can rewind the growing buffer. */
            lv_screenSpecSetLiveMode(TRUE);
        }
    }

    /* Steady loop: pump the transport, drain every record queued this frame into
     * the decoder, pump once more, then render the current mode. The grave key
     * toggles between the block-grid overview and the followed game view; the
     * loop runs off !quit so a mode toggle never exits it. The server sends ~one
     * record per delayed tick, so draining all queued records each frame needs
     * no host-side throttle. Entered only once a seed loaded; a stalled feed
     * overlays "connection lost" until exit. */
    while (!g_lv->quit && g_lv->isLoaded) {
        SDL_Event sdlEvent;
        SpecDrainRecord rec;
        bool drainedAny = false;

        /* The server re-subscribed this spectator to the live lobby control bus
         * (its delayed game drained back across the game→lobby boundary), so
         * live lobby control has begun arriving — return to the caller, which
         * re-enters the live read-only lobby against the now-live ClientSim.
         * This is distinct from the in-stream new-game keyframe handled below,
         * which keeps playing delayed and must not return. */
        if (specDrainLiveResumed(cs)) {
            liveResumed = true;
            break;
        }

        /* Controller input is gated on the leave-confirm modal. While it is
         * open ImGui owns the gamepad (A confirms / B cancels the prompt), so
         * map pan/zoom and our own B-to-leave stand down. Drive the gamepad-nav
         * flag the same way — off in the pannable overview so the stick/d-pad
         * pan instead of moving ImGui focus, on for the modal and the game view
         * — and set it before lvHostRenderFrame's newframe reads it. */
        bool specPopupOpen = lv_imgui_any_popup_open() != 0;
        lv_imgui_set_gamepad_nav((specPopupOpen || g_lv->gameView) ? 1 : 0);

        specDrainPump(cs);
        while (SDL_PollEvent(&sdlEvent)) {
            /* Alt+Enter: full screen. First, so nothing else has the key. */
            if (lvHostHandleFullScreenKey(&sdlEvent)) {
                continue;
            }
            if (sdlEvent.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
                sdlEvent.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
                lvHostHandleFullScreenChange(&sdlEvent);
            }
            /* Reuse the viewer's game-view input. The grave key toggles
             * between the block-grid overview and the game view (allowToggle),
             * and ,-. seek parks the live feed like the scrubber (allowSeek). */
            if (lvHostHandleGameViewKey(sdlEvent, /* allowToggle */ TRUE,
                                        /* allowSeek */ TRUE)) {
                continue;
            }
            /* End / L jumps back to the live head and resumes following it. */
            if (sdlEvent.type == SDL_EVENT_KEY_DOWN &&
                (sdlEvent.key.key == SDLK_END || sdlEvent.key.key == SDLK_L)) {
                lv_screenSpecJumpToLive();
                continue;
            }
            lv_imgui_context_handle_event(&sdlEvent);
            if (sdlEvent.type == SDL_EVENT_RENDER_TARGETS_RESET ||
                sdlEvent.type == SDL_EVENT_RENDER_DEVICE_RESET) {
                lv_drawDirtyScreen();
            }
            if (sdlEvent.type == SDL_EVENT_WINDOW_EXPOSED ||
                sdlEvent.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                lv_drawDirtyScreen();
                g_lv->wantScreenUpdate = TRUE;
            }
            /* Snap + reflow the overview on a drag-resize/maximize, matching
             * the standalone viewer (game view is fixed-size and ignored). */
            if (sdlEvent.type == SDL_EVENT_WINDOW_RESIZED) {
                lvHostHandleResize(&sdlEvent);
            }
            /* Mouse-wheel zoom for the block-grid overview, anchored on the
             * cursor, as in logViewerRun. Skipped while ImGui owns the mouse
             * (cursor over a panel) and in game view (fixed size; 1-4 keys
             * zoom there). */
            if (sdlEvent.type == SDL_EVENT_MOUSE_WHEEL) {
                if (!lv_imgui_want_capture_mouse() && !g_lv->gameView &&
                    !lv_clip_gif_active()) {
                    int mx = (int)sdlEvent.wheel.mouse_x;
                    int my = (int)sdlEvent.wheel.mouse_y;
                    if (sdlEvent.wheel.y > 0.0f) {
                        lv_drawZoomIn(mx, my);
                    } else if (sdlEvent.wheel.y < 0.0f) {
                        lv_drawZoomOut(mx, my);
                    }
                }
            }
            /* Window-X / OS quit leaves immediately — never trap the OS close.
             * Esc instead arms the "Leave spectating?" confirm modal (rendered
             * by lvHostRenderFrame in both overview and game view); the future
             * controller B-button arms it through the same entry point. The
             * game-view key handler above leaves Esc unconsumed, so it falls
             * through here. */
            /* Embedded, this is WinBolo's window the viewer is drawing into;
             * a close request meant for anything else is not ours. */
            if (sdlEvent.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                sdlEvent.window.windowID == SDL_GetWindowID(g_lv->window)) {
                logViewerRequestAppQuit();
            }
            if (sdlEvent.type == SDL_EVENT_QUIT) {
                g_lv->quit = TRUE;
                break;
            }
            if (sdlEvent.type == SDL_EVENT_KEY_DOWN &&
                sdlEvent.key.key == SDLK_ESCAPE) {
                lv_imgui_spectator_leave_request();
            }
            /* Controller B (East) mirrors Esc: arm the "Leave spectating?"
             * confirm. Suppressed while the modal is open so ImGui's own
             * East = cancel resolves it rather than this re-arming it. */
            if (!specPopupOpen &&
                sdlEvent.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                sdlEvent.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                lv_imgui_spectator_leave_request();
            }
        }

        /* Controller pan/zoom for the block-grid overview (game view is
         * fixed-size and ignored; the modal owns input while open). Pan mirrors
         * the mouse-drag conversion — screen-pixel deltas map to 1/zoom zoom-1
         * native pixels — but adds the stick delta to the absolute pan (stick
         * right/down moves the view right/down), the inverse of grab-drag,
         * which subtracts. Zoom is centered on the window, reusing the same
         * lv_drawZoomIn/Out the mouse wheel drives. */
        if (!g_lv->gameView && !specPopupOpen) {
            float panX = 0.0f, panY = 0.0f;
            int   zoomDir = 0;
            if (specInputPollPan(&panX, &panY)) {
                float zoom = lv_drawGetZoomLevel();
                if (zoom <= 0.0f) zoom = 1.0f;
                /* Per-frame pan reach in screen pixels at full deflection. */
                const float kPanSpeedPx = 12.0f;
                unsigned char ox = 0, oy = 0;
                int sx = 0, sy = 0;
                lv_screenGetOffsets(&ox, &oy);
                lv_screenGetSubOffset(&sx, &sy);
                int curX = (int)ox * TILE_SIZE_X + sx;
                int curY = (int)oy * TILE_SIZE_Y + sy;
                int newX = curX + (int)(panX * kPanSpeedPx / zoom);
                int newY = curY + (int)(panY * kPanSpeedPx / zoom);
                lv_drawDirtyScreen();
                lv_screenPanToTotalPixels(newX, newY);
            }
            if (specInputPollZoom(&zoomDir)) {
                int w = 0, h = 0;
                SDL_GetWindowSize(g_lv->window, &w, &h);
                if (zoomDir > 0) {
                    lv_drawZoomIn(w / 2, h / 2);
                } else {
                    lv_drawZoomOut(w / 2, h / 2);
                }
            }
        }

        /* The confirm modal resolves on a click: Yes leaves the feed, No keeps
         * watching (the modal closed itself). */
        if (lv_imgui_spectator_leave_confirmed()) {
            g_lv->quit = TRUE;
        }

        /* Drain all records queued this frame. In live-DVR mode lv_specRecordPump
         * only appends (the stream pump no longer auto-advances); the frame update
         * below drives the decoder forward per follow-live / parked mode. Track the
         * latest record tick so the parked head-time can extrapolate. */
        while (specDrainPopRecord(cs, &rec)) {
            /* The routing is lv_spec_feed.c's: a new segment re-seeds, a
               keyframe refreshes the roster and phase, every record moves the
               live head. What is left here is the window title, which only
               this host has. */
            LvSpecFeedResult fed = lvSpecFeedRecord(&rec);
            if (fed == LV_SPEC_FEED_RESEEDED || fed == LV_SPEC_FEED_PHASE_CHANGED) {
                /* Refresh title + map + phase tag for the new segment, or only
                   when the phase actually changed, so we don't
                   SDL_SetWindowTitle on every keyframe. */
                lvSpecApplyTitle(serverHost, serverPort);
            }
            if (rec.payload != NULL) {
                free(rec.payload);
            }
            drainedAny = true;
        }
        specDrainPump(cs);

        if (drainedAny) {
            lastProgressMs = SDL_GetTicks();
        }
        if (!connectionLost && (SDL_GetTicks() - lastProgressMs) > kSpecStallMs) {
            connectionLost = true;
        }

        /* Advance the decoder: follow-live slams to the head, parked plays forward
         * at real time from the scrub point; pause (playIsPlaying) freezes either.
         * Keeps g_lv->totalTimeMs pointed at the growing live head for the scrubber. */
        lv_screenSpecFrameUpdate(SDL_GetTicks());

        g_lv->wantScreenUpdate = TRUE;

        /* Render the current mode via the shared dispatch. In game view it
         * keeps the Controls panel; in the block-grid overview it draws the
         * viewer's menu bar, sidebars and panels (see lvHostRenderFrame). A
         * stalled feed shows "connection lost" on the frozen last frame until
         * exit. */
        lvHostRenderFrame(connectionLost ? langGetText(STR_LV_SPEC_CONNECTION_LOST) : NULL);

        /* Keep the saved layout no more than a moment behind the screen, as
         * the viewer does. */
        lvHostAutosave();
    }

    /* Leave live-DVR mode so the stream pump / transport revert to standalone
     * behaviour for any later log session in this process. */
    lv_screenSpecSetLiveMode(FALSE);

    /* Restore the default gamepad UI nav the overview disabled while panning,
     * so any later log session in this process keeps controller focus nav. */
    lv_imgui_set_gamepad_nav(1);

    /* Teardown: if the spectator exited while in the game view, restore its
     * window size/tile counts and team-colour state (the grave toggle set them
     * on entry), run the shared shutdown, and drop the seed control stash. The
     * ClientSim/transport belongs to the caller — do not disconnect it here. */
    if (g_lv->gameView) {
        lv_drawGameViewTeardown();
        g_lv->useTeamColours = g_lv->savedUseTeamColours;
        g_lv->gameView = FALSE;
    }
    /* The layout and the options, as the viewer writes them when it closes.
     * After the game view is gone, so its fixed size is not saved. */
    savePreferences();
    lvHostTeardown();
    lv_specSeedControlClear();
    return liveResumed;
}
