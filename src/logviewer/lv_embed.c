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
 * Name:          lv_embed.c
 * Purpose:
 *   The ImGui-free half of the host bring-up and teardown,
 *   shared by the standalone viewer, the spectator and the
 *   embed, plus the embedded reel API: a non-modal
 *   render-to-texture surface a host drives from inside its
 *   own ImGui frame, rather than a loop that takes the
 *   event loop over.
 *********************************************************/

#include <stdlib.h>
#include <string.h>
#include "backend.h"
#include "lv_players.h"   /* lv_playersSetViewByName — reel focus-on-player */
#include "lv_global.h"
#include "clientmutex.h"
#include "draw.h"
#include "sound.h"
#include "dns.h"
#include "tiles.h"
#include "logviewer.h"
#include "lv_host.h"

#include <SDL3/SDL.h>

/* ImGui integration headers */
#include "imgui/imgui_context.h"
#include "imgui/imgui_events.h"
#include "imgui/imgui_comments.h"

/* Platform abstraction */
#include "platform/platform_config.h"
#include "platform/platform_dialogs.h"
#include "../gui/sdl3/macos_pinch.h"
#ifdef __APPLE__
#include "platform/mac_menubar.h"
#endif

/* File-scope pointer to the current viewer session state */
LogViewerState *g_lv = NULL;

/* Panel forwarders lv_windowStop calls. They live in logviewer.c and have no
 * header of their own; screen.c hand-declares the same pair. */
void lv_updateItem(BYTE itemType, BYTE itemNumber, BYTE owner, BYTE x, BYTE y, BYTE armour, BYTE shells, BYTE mines, bool inTank);
void lv_frontEndSetGameInformation(bool clear, BYTE versionMajor, BYTE versionMinor, BYTE versionRevision, char *mapName, BYTE gameType, bool hiddenMines, BYTE aiType, int32_t startDelay, int32_t timeLimit, BYTE *wbnKey, int32_t startTime);

/* --------------------------------------------------------------------------
 * Replay driver
 *
 * Playback speed, the SDL timer pair that steps the decoder, and the
 * play/pause/stop entry points. It sits with the embed rather than with the
 * modal viewer because every build that decodes a log needs it, and one copy
 * serves them all: SDL3 implements SDL_AddTimer / SDL_RemoveTimer on
 * emscripten_set_timeout under Emscripten and fires the callbacks on the main
 * thread from the JS event loop, so the timer bodies are correct in the
 * browser and on the desktop alike.
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * lv_updateSpeed -- sets timerSleep for the given speed value.
 * Called from imgui_controls.cpp via extern, and from mainLoadPreferences.
 * -------------------------------------------------------------------------- */
void lv_updateSpeed(BYTE spd, int updateSlider) {
    (void)updateSlider; /* ImGui slider reads g_lv->speed directly */
    int ts;
    switch (spd) {
    case 2: ts = 18; break;
    case 3: ts = 16; break;
    case 4: ts = 14; break;
    case 5: ts = 12; break;
    case 6: ts = 10; break;
    case 7: ts =  8; break;
    case 8: ts =  6; break;
    case 9: ts =  1; break;
    default:
        ts = 20;
        spd = 1;
        break;
    }
    if (g_lv != NULL) {
        g_lv->speed = spd;
        g_lv->timerSleep = ts;
    }
}

/* --------------------------------------------------------------------------
 * Screen update / redraw
 * -------------------------------------------------------------------------- */
void lv_windowNeedRedraw(void) {
    g_lv->wantScreenUpdate = TRUE;
}

/* --------------------------------------------------------------------------
 * SDL timer callbacks (called on background threads -- only set flags)
 * -------------------------------------------------------------------------- */
Uint32 SDLCALL lv_windowFrameTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
    (void)userdata; (void)timerID; (void)interval;
    g_lv->wantScreenUpdate = TRUE;
    if (g_lv->playIsPlaying == TRUE) {
        return 50;
    }
    return 0;
}

Uint32 SDLCALL lv_windowTimer(void *userdata, SDL_TimerID timerID, Uint32 interval) {
    (void)userdata; (void)timerID; (void)interval;
    lv_clientMutexWaitFor();
    lv_screenLogTick();
    if (g_lv->doubleSpeed == TRUE) {
        lv_screenLogTick();
        lv_screenLogTick();
    }
    lv_clientMutexRelease();
    if (g_lv->playIsPlaying == TRUE) {
        return (Uint32)g_lv->timerSleep;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Playback control
 * -------------------------------------------------------------------------- */
void lv_windowPlay(void) {
    /* Spectator live-DVR runs in the foreground spectatorRun loop, which drives
       the decoder itself — never start the background replay timer here (it would
       double-drive lv_screenLogTick and race the live append). playIsPlaying is
       the orthogonal play/freeze flag the loop reads. */
    if (lv_screenSpecIsLiveMode()) {
        g_lv->playIsPlaying = TRUE;
        return;
    }
    if (g_lv->playIsPlaying == FALSE) {
        g_lv->timerGameID  = SDL_AddTimer(20,  lv_windowTimer,      NULL);
        g_lv->timerFrameID = SDL_AddTimer(50,  lv_windowFrameTimer, NULL);
    }
    g_lv->playIsPlaying = TRUE;
}

void lv_windowPause(void) {
    if (lv_screenSpecIsLiveMode()) {
        g_lv->playIsPlaying = FALSE;
        return;
    }
    lv_clientMutexWaitFor();
    if (g_lv->playIsPlaying == TRUE) {
        SDL_RemoveTimer(g_lv->timerGameID);
        SDL_RemoveTimer(g_lv->timerFrameID);
        g_lv->timerGameID  = 0;
        g_lv->timerFrameID = 0;
    }
    g_lv->playIsPlaying = FALSE;
    lv_clientMutexRelease();
}

void lv_windowStop(int corruptLog) {
    /* Spectator: the panel's Stop button freezes the feed rather than closing the
       live log out from under the foreground host loop. */
    if (lv_screenSpecIsLiveMode()) {
        g_lv->playIsPlaying = FALSE;
        return;
    }
    if (corruptLog == TRUE) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, DIALOG_BOX_TITLE,
                                 "Error: Corrupt Log File", NULL);
    }
    lv_windowPause();
#ifndef __EMSCRIPTEN__
    /* Gives a timer callback that was already running when the pause removed
       its timer time to finish before the log is closed under it. The web
       build is single-threaded, so there is no such callback to wait for, and
       it links without ASYNCIFY, so a delay there would block the tab. */
    SDL_Delay(500);
#endif
    lv_clientMutexWaitFor();
    lv_frontEndSetGameInformation(TRUE, 0, 0, 0, NULL, 0, 0, 0, 0, 0, NULL, 0);
    lv_updateItem(0, 0, 0, 0, 0, 0, 0, 0, FALSE);
    lv_screenCloseLog();
    g_lv->isLoaded = FALSE;
    lv_imgui_events_clear();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
}

/* --------------------------------------------------------------------------
 * Decoder callbacks
 *
 * The two backend callbacks the decode path reaches on its own: one from
 * lv_screenUpdate on every repaint, one from lv_screenLogTick when the log
 * runs out. They sit with the replay driver rather than with the modal viewer
 * for the same reason it does — every build that decodes a log needs them, and
 * one copy serves them all.
 * -------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------
 * Playback drawing
 * -------------------------------------------------------------------------- */
void lv_frontEndDrawMainScreen(screen *value, screenMines *mineView, screenTanks *tks,
                            screenGunsight *gs, screenBullets *sBullet, screenLgm *lgms,
                            int32_t srtDelay, bool isPillView, int edgeX, int edgeY) {
    lv_drawMainScreen(value, mineView, tks, gs, sBullet, lgms,
                   FALSE, FALSE, srtDelay, isPillView, edgeX, edgeY, FALSE, 0, 0);
}

/* --------------------------------------------------------------------------
 * End-of-log callback from backend
 * -------------------------------------------------------------------------- */
void lv_finished(void) {
    g_lv->playIsPlaying = FALSE;
}

/* --------------------------------------------------------------------------
 * lvHostSetupCore -- ImGui-free half of the host bring-up.
 *
 * Allocates g_lv and brings up the platform layer, mutex, draw module and
 * dialog window association. Everything here is safe with no ImGui context of
 * the viewer's own, so the embedded reel shares it with the standalone viewer
 * and the spectator. Returns TRUE on success; on any failure it unwinds
 * whatever it brought up and returns FALSE with g_lv cleared.
 * -------------------------------------------------------------------------- */
bool lvHostSetupCore(SDL_Window *window, SDL_Renderer *renderer,
                     bool fromMainMenu) {
    char line[256];
    int  sizeX, sizeY;

    /* Allocate central logviewer state */
    g_lv = lv_decoderCreate(fromMainMenu);
    if (g_lv == NULL) {
        return FALSE;
    }

    /* Platform abstraction init */
    lv_platform_config_init("WinBolo");
    lv_platform_dialogs_init();

    if (lv_clientMutexCreate() == FALSE) {
        lv_platform_dialog_error("WinBolo Log Viewer", "Could not create mutex");
        if (window == NULL) { lv_platform_config_shutdown(); lv_platform_dialogs_shutdown(); }
        free(g_lv);
        g_lv = NULL;
        return FALSE;
    }

    /* Load screen size before setting up draw */
    lv_platform_config_get_string("LOGVIEWER", "ScreenSizeX", "50", line, sizeof(line));
    sizeX = atoi(line);
    if (sizeX < 5 || sizeX > 99) sizeX = 50;

    lv_platform_config_get_string("LOGVIEWER", "ScreenSizeY", "38", line, sizeof(line));
    sizeY = atoi(line);
    if (sizeY < 5 || sizeY > 99) sizeY = 38;

    lv_screenSetSizeX((BYTE)sizeX);
    lv_screenSetSizeY((BYTE)sizeY);

    if (window == NULL) {
        /* Standalone mode: create own window/renderer */
        g_lv->ownsWindow = TRUE;
        if (lv_drawSetup() == FALSE) {
            lv_clientMutexDestroy();
            lv_platform_config_shutdown();
            lv_platform_dialogs_shutdown();
            free(g_lv);
            g_lv = NULL;
            return FALSE;
        }
        g_lv->window = lv_drawGetSDLWindow();
        g_lv->renderer = lv_drawGetSDLRenderer();
        /* Standalone: init audio subsystem (embedded gets it from main app) */
        SDL_InitSubSystem(SDL_INIT_AUDIO);
    } else {
        /* Embedded mode: borrow caller's window/renderer */
        g_lv->window = window;
        g_lv->renderer = renderer;
        g_lv->ownsWindow = FALSE;
        if (lv_drawSetupWithHandles(window, renderer) == FALSE) {
            lv_clientMutexDestroy();
            free(g_lv);
            g_lv = NULL;
            return FALSE;
        }
    }

    /* Associate the window with dialogs (needed on Linux/Wayland for portal) */
    lv_platform_dialogs_set_window(g_lv->window);

    /* macOS trackpad pinch-to-zoom (no-op if already initialised or non-macOS) */
    macOSPinchZoomInit();

    return TRUE;
}

/* --------------------------------------------------------------------------
 * lvHostTeardownCommon -- Host shutdown up to (but not including) releasing
 * g_lv.
 *
 * Stops the decoder, tears down draw / mutex / DNS and (when standalone) the
 * platform layer. withImGui also tears down the viewer's ImGui context, its
 * comments panel, the native menu bar and sound — the pieces the embedded reel
 * never brought up. Ordering matches the standalone shutdown exactly.
 * -------------------------------------------------------------------------- */
void lvHostTeardownCommon(bool withImGui) {
    lv_windowStop(FALSE);
    if (withImGui) {
#ifdef __APPLE__
        /* Restore the previously-installed NSMenu (WinBolo's, when embedded;
         * empty stub when standalone since the process is exiting). */
        lv_mac_menubar_uninstall();
#endif
        lv_imgui_comments_shutdown();
        lv_imgui_context_shutdown();
    }
    lv_drawCleanupSplash();
    if (withImGui) {
        lv_soundCleanup();
    }
    {
        bool standalone = g_lv->ownsWindow;
        lv_drawCleanup();
        lv_clientMutexDestroy();
        lv_dnsShutdown();
        if (standalone) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            lv_platform_config_shutdown();
            lv_platform_dialogs_shutdown();
        }
    }
}

/* --------------------------------------------------------------------------
 * Embedded reel API
 *
 * Drives the decoder and the block-grid render-to-texture for a host that
 * already owns an ImGui frame, so the round can be drawn as a texture inside
 * that frame. Only the ImGui-free half of the host is brought up: no viewer
 * ImGui context, no panels, no sound, no preferences (saving them would
 * persist the host's panel-sized tile counts as the standalone viewer's
 * window size). There is one decoder singleton in the process, so an embed
 * and a full viewer/spectator session can never be live at the same time.
 *
 * Signatures carry scalars and void * only: the host hand-declares them
 * instead of including this header, whose backend types collide with the
 * client's.
 * -------------------------------------------------------------------------- */
static bool s_embedActive = false;

/* Pan latch, in zoom-1 native pixels, taken when the drag starts. */
static int s_embedPanStartPxX = 0;
static int s_embedPanStartPxY = 0;

/* Zoom step the embed found on entry, handed back on teardown. Only ever read
 * after lvEmbedBegin has stored it. */
static int s_embedSavedZoomStep = 0;

/* The player watching the reel, by name — the only key the host shares with the
 * log. Empty when the host named nobody (a spectator has no slot of their own),
 * and the reel then draws exactly as it did before it could be told. */
static char s_embedSelfName[PLAYER_NAME_LEN];

bool lvEmbedBegin(SDL_Window *window, SDL_Renderer *renderer,
                  uint8_t *zipData, size_t zipLen, int viewW, int viewH) {
    /* The buffer is ours from the call on (lv_screenLoadMapFromMemory takes
     * ownership even when it fails), so every refusal here releases it. */
    if (window == NULL || renderer == NULL || zipData == NULL || zipLen == 0 ||
        g_lv != NULL) {
        if (zipData != NULL) {
            free(zipData);
        }
        return false;
    }

    lv_drawSetEmbedded(1);
    lv_drawSetEmbedViewport(viewW, viewH);
    if (lvHostSetupCore(window, renderer, /* fromMainMenu */ false) == FALSE) {
        lv_drawSetEmbedded(0);
        free(zipData);
        return false;
    }
    s_embedActive = true;

    /* Defaults the skipped halves would otherwise supply: no sound
     * (lv_soundSetup never ran), the reel always opens at game start rather
     * than inheriting the viewer's hide-lobby preference, and a non-zero
     * timerSleep — lv_decoderCreate callocs it to 0 and lv_windowTimer
     * cancels its own SDL timer when it returns 0, so playback would stop
     * after a single tick. */
    g_lv->isSoundsPlaying = FALSE;
    lv_screenSetHideLobby(1);
    lv_updateSpeed(1, FALSE);
    lv_imgui_events_clear();
    s_embedSelfName[0] = '\0';

    /* Open at the widest zoom step (0.5x) so a panel-sized rect shows a
     * useful slice of the map, then size the tile grid to that rect. The step
     * is a process-wide static, so save what was there for the teardown to
     * put back — otherwise the next full-window viewer session opens at the
     * reel's zoom. */
    s_embedSavedZoomStep = lv_drawGetZoomStepIndex();
    lv_drawSetZoomStep(0, viewW / 2, viewH / 2);
    lvEmbedSetViewportSize(viewW, viewH);

    if (lv_screenLoadMapFromMemory(zipData, zipLen) == FALSE) {
        lvEmbedEnd();
        return false;
    }
    g_lv->isLoaded = TRUE;
    lv_windowNeedRedraw();
    lv_windowPlay();
    return true;
}

void lvEmbedEnd(void) {
    if (!s_embedActive) {
        return;
    }
    lvHostTeardownCommon(false);
    /* lv_decoderDestroy rather than the standalone path's bare free(): it
     * closes the log and clears the decoder's active-state pointer, which
     * matters when the host tears an embed down every round. */
    lv_decoderDestroy(g_lv);
    g_lv = NULL;
    s_embedActive = false;
    lv_drawSetEmbedded(0);
    /* Hand the zoom step back to whatever session runs next. The raw setter,
     * because the anchored path resizes the tile grid through decoder state
     * that no longer exists. */
    lv_drawSetZoomStepIndexRaw(s_embedSavedZoomStep);
}

bool lvEmbedIsActive(void) {
    return s_embedActive;
}

/* Name the player the reel is being watched by. Everything the decoder bakes
 * relative to "self" — the tanks' good/evil frames, and the good/evil/neutral
 * pill and base tiles — then describes that player rather than log slot 0,
 * whoever that happened to be, and the tanks are drawn green for their allies
 * and red for their enemies instead of from the team palette the reel loads no
 * preferences for. An empty name (a spectator, watching a round they had no
 * slot in) leaves both alone. */
void lvEmbedSetSelfName(const char *name) {
    if (!s_embedActive || g_lv == NULL) {
        return;
    }
    if (name == NULL) {
        name = "";
    }
    lv_clientMutexWaitFor();
    strncpy(s_embedSelfName, name, sizeof(s_embedSelfName) - 1);
    s_embedSelfName[sizeof(s_embedSelfName) - 1] = '\0';
    g_lv->allyColours = (s_embedSelfName[0] != '\0');
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
}

/* Re-fit the tile grid to a host rect that changed size. Mirrors
 * lvHostHandleResize without the window snap (the host owns the window) and
 * with no menu bar. The decode timers are live, so the mutation is held under
 * lv_clientMutex. */
void lvEmbedSetViewportSize(int viewW, int viewH) {
    float zoom, tilePxX, tilePxY;
    int   newTilesX, newTilesY;

    if (!s_embedActive || g_lv == NULL) {
        return;
    }
    lv_drawSetEmbedViewport(viewW, viewH);

    zoom = lv_drawGetZoomLevel();
    if (zoom <= 0.0f) zoom = 1.0f;
    tilePxX = (float)TILE_SIZE_X * zoom;
    tilePxY = (float)TILE_SIZE_Y * zoom;
    newTilesX = (int)(((float)viewW + tilePxX * 0.5f) / tilePxX);
    newTilesY = (int)(((float)viewH + tilePxY * 0.5f) / tilePxY);
    if (newTilesX < 1) newTilesX = 1;
    if (newTilesY < 1) newTilesY = 1;
    if (newTilesX > 255) newTilesX = 255;
    if (newTilesY > 255) newTilesY = 255;

    if ((BYTE)newTilesX == lv_screenGetSizeX() &&
        (BYTE)newTilesY == lv_screenGetSizeY()) {
        return;
    }

    lv_clientMutexWaitFor();
    /* The size setters keep the offset inside the map for the new grid. The
     * sub-pixel pan is reset here because a resize carries no in-flight
     * drag. */
    lv_screenSetSizeX((BYTE)newTilesX);
    lv_screenSetSizeY((BYTE)newTilesY);
    lv_screenSetSubOffset(0, 0);
    lv_drawResizeRenderTarget();
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    g_lv->wantScreenUpdate = TRUE;
}

/* Bring the render target up to date when the decode timers asked for it, and
 * hand back the texture plus the visible slice within it. Never clears and
 * never presents — the host's frame owns the framebuffer. */
/* Sub-tile blit origin latched at the moment the render target was last
 * painted — see the note in lvEmbedFrameTexture. Only ever written under
 * lv_clientMutex. */
static int s_embedSubX = 0;
static int s_embedSubY = 0;

bool lvEmbedFrameTexture(void **outTexture, int *outTexW, int *outTexH,
                         int *outSrcX, int *outSrcY, int *outSrcW, int *outSrcH) {
    SDL_Texture *tex;
    int subX = 0, subY = 0;

    if (!s_embedActive || g_lv == NULL || g_lv->isLoaded == FALSE) {
        return false;
    }

    /* Latch the sub-tile offset WITH the render, under the same lock.
     *
     * lv_screenLogTick runs on the SDL timer thread (lv_windowTimer, 20 ms)
     * and moves the camera there — lv_screenFollowCentredTank writes xOffset
     * and subPxX/subPxY every tick. Reading subPx after releasing the mutex
     * could therefore pick up a camera one or more ticks newer than the tile
     * grid baked into the render target: when the camera crossed a tile
     * boundary in that gap, xOffset had advanced a tile while the texture
     * still held the old grid, so the blit's source origin jumped a whole tile
     * the wrong way and corrected on the next frame — the reel's flicker.
     *
     * Latching inside the lock makes the offset handed out always describe the
     * content actually in the texture. On frames that do not re-render, the
     * previous latch still matches the texture, which is exactly right:
     * stale-but-matched is correct, fresh-but-torn is the bug. */
    lv_clientMutexWaitFor();
    if (g_lv->wantScreenUpdate == TRUE) {
        /* Point self at the watching player for the length of the build, the
         * way the viewer's game view points it at the camera tank: the tank,
         * pill and base sprite indices are baked from lv_playersGetSelf here,
         * so without it they describe log slot 0. Restored straight after
         * because self doubles as the follow camera's target, which the host
         * moves with lvEmbedFocusPlayerByName — clicking a name to follow
         * somebody must not repaint the round from their side. */
        BYTE savedSelf = lv_playersGetSelf();
        BYTE watching = lv_playersFindByName(s_embedSelfName);
        if (watching != NEUTRAL) {
            lv_playersSetSelf(watching);
        }
        lv_screenUpdate(redraw);
        lv_playersSetSelf(savedSelf);
        g_lv->wantScreenUpdate = FALSE;
        lv_screenGetSubOffset(&s_embedSubX, &s_embedSubY);
    }
    subX = s_embedSubX;
    subY = s_embedSubY;
    lv_clientMutexRelease();

    tex = lv_drawGetGameTexture();
    if (tex == NULL) {
        return false;
    }
    if (outTexture != NULL) *outTexture = tex;
    lv_drawGetGameTargetSize(outTexW, outTexH);
    if (outSrcX != NULL) *outSrcX = subX;
    if (outSrcY != NULL) *outSrcY = subY;
    if (outSrcW != NULL) *outSrcW = lv_screenGetSizeX() * TILE_SIZE_X;
    if (outSrcH != NULL) *outSrcH = lv_screenGetSizeY() * TILE_SIZE_Y;
    return true;
}

/* Scale the slice reported by lvEmbedFrameTexture is meant to be drawn at:
 * one source pixel becomes this many host pixels. 1.0 while no embed is
 * running, so a host that asks too early still gets a usable number. */
float lvEmbedGetZoomLevel(void) {
    float zoom;

    if (!s_embedActive || g_lv == NULL) {
        return 1.0f;
    }
    zoom = lv_drawGetZoomLevel();
    return (zoom > 0.0f) ? zoom : 1.0f;
}

void lvEmbedPlay(void) {
    if (!s_embedActive || g_lv == NULL || g_lv->isLoaded == FALSE) {
        return;
    }
    lv_windowPlay();
}

void lvEmbedPause(void) {
    if (!s_embedActive || g_lv == NULL) {
        return;
    }
    lv_windowPause();
}

bool lvEmbedIsPlaying(void) {
    return s_embedActive && g_lv != NULL && g_lv->playIsPlaying == TRUE;
}

/* Cursor-anchored wheel zoom. Coordinates are image-local; embed mode forces
 * the menu-bar offset to 0, so they need no adjustment. The zoom reallocates
 * the screen buffer and the render target, so it takes the mutex. */
void lvEmbedWheel(int localX, int localY, float wheelY) {
    if (!s_embedActive || g_lv == NULL) {
        return;
    }
    lv_clientMutexWaitFor();
    if (wheelY > 0.0f) {
        lv_drawZoomIn(localX, localY);
    } else if (wheelY < 0.0f) {
        lv_drawZoomOut(localX, localY);
    }
    lv_clientMutexRelease();
}

void lvEmbedPanBegin(void) {
    BYTE ox = 0, oy = 0;
    int  sx = 0, sy = 0;

    if (!s_embedActive || g_lv == NULL) {
        return;
    }
    /* Both halves of the camera under one lock. A replay tick on the SDL timer
     * thread can land between the two reads, and the pair is exactly where
     * that hurts: at a tile crossing the tile index advances while subPx wraps
     * 15 -> 0, so a torn pair anchors the drag a full tile away from where the
     * view actually is and the whole drag is offset by 16 px.
     *
     * Live values, not the render-time snapshot: a drag anchors to where the
     * camera IS, not to what the last painted texture shows. */
    lv_clientMutexWaitFor();
    lv_screenGetOffsets(&ox, &oy);
    lv_screenGetSubOffset(&sx, &sy);
    lv_clientMutexRelease();
    s_embedPanStartPxX = (int)ox * TILE_SIZE_X + sx;
    s_embedPanStartPxY = (int)oy * TILE_SIZE_Y + sy;
}

/* Drag delta in host screen pixels, measured from where lvEmbedPanBegin
 * latched. Each on-screen pixel is 1/zoom native pixels, and dragging right
 * reveals more of the map's left, so the delta is subtracted. */
void lvEmbedPanDelta(float dxScreenPx, float dyScreenPx) {
    float zoom;
    int   totalPxX, totalPxY;

    if (!s_embedActive || g_lv == NULL) {
        return;
    }
    zoom = lv_drawGetZoomLevel();
    if (zoom <= 0.0f) zoom = 1.0f;
    totalPxX = s_embedPanStartPxX - (int)(dxScreenPx / zoom);
    totalPxY = s_embedPanStartPxY - (int)(dyScreenPx / zoom);

    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenPanToTotalPixels(totalPxX, totalPxY);
    lv_clientMutexRelease();
    g_lv->wantScreenUpdate = TRUE;
}

/* Elapsed and total milliseconds of the presented window. An embed hides the
 * lobby, so that window is the game portion of the round. Either out-param may
 * be NULL; both read 0 while no embed is running, so a host can drive its
 * transport without checking first. */
void lvEmbedGetProgress(uint32_t *outCurMs, uint32_t *outTotalMs) {
    size_t   pos = 0, size = 0;
    uint32_t cur = 0, total = 0;

    if (s_embedActive && g_lv != NULL && g_lv->isLoaded == TRUE) {
        lv_screenGetLogProgress(&pos, &size, &cur, &total);
    }
    if (outCurMs != NULL) {
        *outCurMs = cur;
    }
    if (outTotalMs != NULL) {
        *outTotalMs = total;
    }
}

/* The seeks below walk the log and rebuild the world — the same state
 * lv_windowTimer ticks on the SDL timer thread under lv_clientMutex — so
 * playback is stopped and the lock held across the seek exactly as the
 * standalone scrubber does. Without that, a seek during playback lands
 * lv_shellsAddItem and lv_shellsDestroy on the same list from two threads and
 * corrupts the heap. wantScreenUpdate is what makes lvEmbedFrameTexture
 * repaint, so a reel seeked while paused still shows the new moment. */
void lvEmbedSeekRatio(float ratio) {
    unsigned char wasPlaying;

    if (!s_embedActive || g_lv == NULL || g_lv->isLoaded == FALSE) {
        return;
    }
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    wasPlaying = g_lv->playIsPlaying;
    if (wasPlaying) {
        lv_windowPause();
    }
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToPosition(ratio);
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
    if (wasPlaying) {
        lv_windowPlay();
    }
    g_lv->wantScreenUpdate = TRUE;
}

/* Jump to a moment measured from the start of the presented window, optionally
 * centring the view on the cell it happened at. Callers pass a round-relative
 * time and the window origin is added here, so if the round clock and the log
 * clock ever turn out to disagree this is the single place that gets the
 * correction. No camera re-init afterwards: that pans to the camera tank, which
 * is only wanted in the viewer's game view and would undo the centring. */
static void lvEmbedSeekWindowMs(uint32_t roundRelMs, bool centreOnCell,
                                int mapX, int mapY) {
    unsigned char wasPlaying;

    if (!s_embedActive || g_lv == NULL || g_lv->isLoaded == FALSE) {
        return;
    }
    wasPlaying = g_lv->playIsPlaying;
    if (wasPlaying) {
        lv_windowPause();
    }
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToTimeMs(lv_screenWindowStartMs() + roundRelMs);
    if (centreOnCell) {
        lv_screenCentreOnCell(mapX, mapY);
    }
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
    if (wasPlaying) {
        lv_windowPlay();
    }
    g_lv->wantScreenUpdate = TRUE;
}

/* Point the reel at a named player: adopt them as the followed tank, turn the
 * follow camera on, and move to them now rather than waiting for the next
 * decoded tick (a paused reel would otherwise not move at all).
 *
 * Name is the key because the lobby's slot numbering and the log's player
 * numbering are separate spaces and the name is what both carry. False when
 * nobody matches or the match has no tank on the map at this point in the
 * replay — dead, or not yet joined — and the caller leaves the view where the
 * player had it rather than jumping somewhere arbitrary. */
bool lvEmbedFocusPlayerByName(const char *name) {
    bool found;

    if (!s_embedActive || g_lv == NULL || g_lv->isLoaded == FALSE) {
        return false;
    }
    lv_clientMutexWaitFor();
    found = lv_playersSetViewByName(name);
    if (found) {
        g_lv->centredTank = TRUE;
        lv_screenFollowCentredTank();
        lv_drawDirtyScreen();
    }
    lv_clientMutexRelease();
    if (found) {
        lv_windowNeedRedraw();
        g_lv->wantScreenUpdate = TRUE;
    }
    return found;
}

/* A clip names a place as well as a moment, so the view follows it there. */
void lvEmbedSeekToClip(uint32_t roundRelMs, int mapX, int mapY) {
    lvEmbedSeekWindowMs(roundRelMs, true, mapX, mapY);
}

/* A timestamp names only a moment — the view stays where the player left it. */
void lvEmbedSeekToTime(uint32_t roundRelMs) {
    lvEmbedSeekWindowMs(roundRelMs, false, 0, 0);
}

/* Walk the log forward by whole ticks with no timer driving it, for a host that
 * wants the round a fixed step at a time rather than in real time. Same list
 * lv_windowTimer walks on the SDL timer thread, so the mutex is held across the
 * whole step exactly as it is there — a caller that paused first still shares
 * the decode lists with a frame timer that may not have retired yet. Marking
 * the screen stale is what makes the next lvEmbedFrameTexture repaint, so the
 * caller sees the moment it stepped to. */
void lvEmbedStepTicks(int ticks) {
    int i;

    if (!s_embedActive || g_lv == NULL || g_lv->isLoaded == FALSE || ticks <= 0) {
        return;
    }
    lv_clientMutexWaitFor();
    for (i = 0; i < ticks; i++) {
        lv_screenLogTick();
    }
    lv_clientMutexRelease();
    g_lv->wantScreenUpdate = TRUE;
}
