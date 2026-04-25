/*
 * override_mode — Ctrl-O hitbox debug overlay state
 *
 * Toggled with Ctrl-O.  When active, overlay_drawAfterWorld() draws
 * hitbox outlines (yellow ±128 wu for tanks, 1 tile for pills/bases)
 * and sub-pixel orange dots at each shell's and LGM's authoritative
 * (q->x, q->y) position over the rendered world view.
 *
 * Rendering otherwise stays on the classic mapViewDrawShells/Tanks/
 * LGMs path so what you see is exactly the game client's render.
 *
 * TODO: free camera, scroll-wheel zoom up to 6x, world-grid overlay,
 * Ctrl-[ / Ctrl-] speed control.  Tracked in progress.txt.
 */

#ifndef OVERRIDE_MODE_H
#define OVERRIDE_MODE_H

#include <stdbool.h>
#include <SDL3/SDL.h>

struct gameSim;     /* forward */
struct ClientSim;

#ifdef __cplusplus
extern "C" {
#endif

/* Toggle override mode on/off. Bound to Ctrl-O in the SDL event loop. */
void overrideModeToggle(void);

/* Returns true iff override mode is currently on. */
bool overrideModeIsOn(void);

/* Game-speed slowdown.  Bound to Ctrl-[ (slower) and Ctrl-] (faster).
 * Adds an extra ms delay per frame; capped at 0 (= no extra delay,
 * normal speed) on the upper end so playing fast is impossible.
 * Each Ctrl-[ adds 10 ms; each Ctrl-] subtracts 10 ms. */
void overrideModeSlower(void);
void overrideModeFaster(void);
int  overrideModeExtraDelayMs(void);

/* Arrow-key world panning while override mode is on.  Applied in
 * sdl3draw as a sub-tile screen-pixel offset on edgeX/edgeY (same
 * mechanism the tablet drag-scroll uses), so the camera still
 * follows the player tank but the visible region is shifted.
 * overrideModePan() adds a delta in screen pixels.
 * overrideModeGetPanOffset() reads the current offset for sdl3draw. */
void overrideModePan(int dx, int dy);
void overrideModeGetPanOffset(int *outDx, int *outDy);
void overrideModeResetPan(void);

/* Draw hitbox/sub-pixel overlays for the world view. Called from
 * sdl3draw.c after mapViewDrawLGMs.  No-op when override mode is off.
 *
 * camera/origin params match the same conventions sdl3draw.c already
 * passes to mapViewDrawShells: originX/Y is world view top-left in
 * screen coords, tileW/tileH are screen pixels per tile, edgeX/Y is
 * the sub-tile camera offset, zoomFactor is the global gZoomFactor.
 */
void overrideModeDrawOverlays(SDL_Renderer *renderer,
                              struct ClientSim *cs,
                              int originX, int originY,
                              int tileW, int tileH,
                              int edgeX, int edgeY,
                              int zoomFactor);

#ifdef __cplusplus
}
#endif

#endif /* OVERRIDE_MODE_H */
