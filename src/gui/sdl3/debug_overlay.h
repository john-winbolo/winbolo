/*
 * debug_overlay.h — F1: main-window hitbox overlays, F2: zoom window
 *
 * Include this with extern "C" guards so sdl3imgui.cpp can use it.
 */

#ifndef DEBUG_OVERLAY_H
#define DEBUG_OVERLAY_H

#include <stdbool.h>
#include <SDL3/SDL.h>
/* Include global.h before types.h so WinSock2.h/winnt.h are pulled in at
 * default pack alignment, before types.h does #pragma pack(push,4). */
#include "../../bolo/global.h"
#include "../../bolo/types.h"
#include "../../bolo/screen.h"
#include "../../bolo/client_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Toggle main-window overlay drawing (F1). */
void debugOverlayToggle(void);
bool debugOverlayIsEnabled(void);

/* Called from sdl3DrawMainScreen after mapViewDrawLGMs, still inside clip rect.
 * All coordinate params come from sdl3DrawMainScreen's locals/params. */
void debugOverlayDrawMain(SDL_Renderer *renderer, SDL_Texture *tilesTex,
                          int sheetScale, int zoomFactor,
                          int originX, int originY, int tileW, int tileH,
                          int edgeX, int edgeY,
                          BYTE xOffset, BYTE yOffset,
                          BYTE playerNum, screenGunsight *gs);

/* Toggle zoom window (F2). */
void debugZoomToggle(void);

/* Capture the main window pixels for the zoom window (call BEFORE
 * SDL_RenderPresent on the main renderer). Returns NULL if zoom is closed
 * or on failure. Caller must SDL_DestroySurface() the result. */
SDL_Surface *debugZoomCaptureIfOpen(SDL_Renderer *mainRenderer);

/* Render one frame of the zoom window from a previously captured surface.
 * Called from winbolo.c main loop AFTER SDL_RenderPresent on the main window.
 * captured may be NULL (function is a no-op). */
void debugZoomRenderFrame(SDL_Surface *captured);

/* Route a raw SDL event to the zoom window. Returns true if consumed.
 * Called from sdl3imgui.cpp event loop with the pre-coordinate-conversion
 * raw event so mouse coords are in the zoom window's own space. */
bool debugZoomHandleEvent(const SDL_Event *ev);

/* Destroy zoom window resources. Call before sdl3DrawCleanup(). */
void debugZoomCleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_OVERLAY_H */
