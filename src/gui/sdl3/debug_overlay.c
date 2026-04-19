/*
 * debug_overlay.c — Debug hitbox overlays and zoom window for WinBolo.exe
 *
 * F1: toggle main-window overlays (tank bbox, pillbox tiles, shell boxes,
 *     gunsight sprite, aim line)
 * F2: toggle a separate resizable zoom window that shows a cropped+scaled
 *     copy of the main game view (bitblt from the main renderer, before
 *     present). Scroll to zoom 1-16x, middle-drag to pan, R to re-center.
 */

#include "debug_overlay.h"
#include "sdl3draw.h"
#include "../tiles.h"
#include "../../bolo/global.h"
#include "../../bolo/tank.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/shells.h"
#include "../../bolo/util.h"
#include "../../server/server_sim.h"
#include "../gamefront.h"
#include <stdio.h>

/* -------------------------------------------------------
 * Internal state
 * ------------------------------------------------------- */

static bool gDbgOverlaysEnabled = false;

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    int           zoomFactor;   /* 1..16, default 4 */
    bool          open;
    /* Pan offset in source (main-window) pixels from the frame center.
     * panX > 0 = looking at content to the right of center. */
    float         panX, panY;
    bool          dragging;
    float         dragStartMouseX, dragStartMouseY;
    float         dragStartPanX,  dragStartPanY;
    int           winW, winH;
    /* Source rect of the last rendered frame — needed to map zoom-window
     * mouse coords back to main-window pixel coords for event forwarding. */
    float         srcX, srcY;
    int           capW, capH;
} DebugZoomState;

static DebugZoomState gZoom = {
    NULL, NULL,
    4,     /* zoomFactor */
    false, /* open */
    0.0f, 0.0f,  /* panX, panY */
    false, /* dragging */
    0.0f, 0.0f, 0.0f, 0.0f,
    800, 600
};

/* -------------------------------------------------------
 * 1x overlay render target
 * ------------------------------------------------------- */

/* Overlay elements are drawn at 1x game resolution (1 texture pixel =
 * 1 game pixel = 16 world units) into gOverlayTex, then blitted to the
 * screen scaled by zoomFactor with SDL_SCALEMODE_NEAREST.  This guarantees
 * that a 1-texture-pixel line becomes exactly 1 game pixel (zoomFactor
 * screen pixels) at any zoom level, with no sub-pixel rounding. */
static SDL_Texture *gOverlayTex  = NULL;
static int          gOverlayTexW = 0;
static int          gOverlayTexH = 0;

/* Convert a world coord (256 units/tile) to a 1x game-pixel position in the
 * overlay texture.  Applies TANK_SUBTRACT (128) — confirmed to give the
 * correct sprite/collision-point position for both tanks and shells.
 *
 * orig1xX = originX / zoomFactor,  edge1xX = edgeX / zoomFactor  (integers
 * because originX and edgeX are always multiples of zoomFactor in practice). */
static void worldTo1x(WORLD wx, WORLD wy,
                       int orig1xX, int orig1xY,
                       int edge1xX, int edge1xY,
                       BYTE xOffset, BYTE yOffset,
                       int *ox, int *oy) {
    int dx = (int)wx - 128;   /* TANK_SUBTRACT */
    int dy = (int)wy - 128;
    int bbx = ((dx >> 8) - (int)xOffset) * 16 + ((dx >> 4) & 0xF);
    int bby = ((dy >> 8) - (int)yOffset) * 16 + ((dy >> 4) & 0xF);
    *ox = orig1xX - 8 + bbx - edge1xX;
    *oy = orig1xY - 8 + bby - edge1xY;
}

/* -------------------------------------------------------
 * Main-window overlay toggle
 * ------------------------------------------------------- */

void debugOverlayToggle(void) {
    gDbgOverlaysEnabled = !gDbgOverlaysEnabled;
}

bool debugOverlayIsEnabled(void) {
    return gDbgOverlaysEnabled;
}

/* -------------------------------------------------------
 * Main-window overlay drawing
 * ------------------------------------------------------- */

void debugOverlayDrawMain(SDL_Renderer *renderer, SDL_Texture *tilesTex,
                          int sheetScale, int zoomFactor,
                          int originX, int originY, int tileW, int tileH,
                          int edgeX, int edgeY,
                          BYTE xOffset, BYTE yOffset,
                          BYTE playerNum, screenGunsight *gs,
                          tank *playerTank) {
    ServerSim *ss = gameFrontGetServerSim();
    if (!ss) return;

    /* 1x game-pixel divisors (originX and edgeX are always multiples of
     * zoomFactor since they are computed from tile/zoom constants). */
    int orig1xX = originX / zoomFactor;
    int orig1xY = originY / zoomFactor;
    int edge1xX = edgeX  / zoomFactor;
    int edge1xY = edgeY  / zoomFactor;

    /* Get renderer output size and (re)create the 1x overlay texture when
     * the size or zoom factor has changed. */
    int screenW = 0, screenH = 0;
    SDL_GetRenderOutputSize(renderer, &screenW, &screenH);
    /* Add 2 pixels of headroom so the right/bottom edges are never clipped
     * when screenW is not exactly divisible by zoomFactor. */
    int texW = screenW / zoomFactor + 2;
    int texH = screenH / zoomFactor + 2;

    if (!gOverlayTex || gOverlayTexW != texW || gOverlayTexH != texH) {
        if (gOverlayTex) SDL_DestroyTexture(gOverlayTex);
        gOverlayTex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, texW, texH);
        if (!gOverlayTex) return;
        SDL_SetTextureScaleMode(gOverlayTex, SDL_SCALEMODE_NEAREST);
        gOverlayTexW = texW;
        gOverlayTexH = texH;
    }

    /* Switch to 1x overlay render target.
     * SDL3 saves/restores per-target viewport & clip rect automatically. */
    SDL_SetRenderTarget(renderer, gOverlayTex);
    /* BLENDMODE_NONE stores RGBA values directly (no pre-multiplication).
     * When the overlay is blitted to screen with BLENDMODE_BLEND the stored
     * alpha is used for correct 50% transparency. */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    /* 1. Tank bounding box (yellow, 50% alpha)
     *
     * Hitbox: abs(tank.x - shell.x) < 128  (strict <)
     * Farthest INCLUDED world offset: ±127 = ±7 game pixels.
     * Draw inside-edge box at ±7 so border pixels are the outermost
     * included game-pixel column. */
    WORLD twx = 0, twy = 0;
    tankGetWorld(&ss->sim.tanks[playerNum], &twx, &twy);
    int tcx, tcy;
    worldTo1x(twx, twy, orig1xX, orig1xY, edge1xX, edge1xY, xOffset, yOffset, &tcx, &tcy);
    {
        SDL_SetRenderDrawColor(renderer, 255, 220, 0, 128);
        SDL_FRect r = { (float)(tcx - 7), (float)(tcy - 7), 14.0f, 14.0f };
        SDL_RenderRect(renderer, &r);
        /* Center dot: 1 game pixel */
        SDL_FRect dot = { (float)tcx, (float)tcy, 1.0f, 1.0f };
        SDL_RenderFillRect(renderer, &dot);
        /* Tank angle text below center */
        TURNTYPE tAngle = tankGetAngle(&ss->sim.tanks[playerNum]);
        char angBuf[16];
        SDL_snprintf(angBuf, sizeof(angBuf), "a=%.2f", (double)tAngle);
        SDL_SetRenderDrawColor(renderer, 255, 220, 0, 220);
        SDL_RenderDebugText(renderer, (float)(tcx + 2), (float)(tcy + 9), angBuf);
        int dxShell, dyShell;
        utilCalcDistance(&dxShell, &dyShell, tAngle, SHELL_SPEED);
        char dxyBuf[20];
        SDL_snprintf(dxyBuf, sizeof(dxyBuf), "d=%d,%d", dxShell, dyShell);
        SDL_RenderDebugText(renderer, (float)(tcx + 2), (float)(tcy + 18), dxyBuf);
    }

    /* 2. Pillbox tile boxes (red, 50% alpha)
     *
     * Hitbox: shell.x >> 8 == pill.x  (tile equality)
     * The tile covers [pill.x*256, pill.x*256+255] — 16 game pixels — so
     * ±8 game pixels from the tile center gives the correct inside edge. */
    {
        BYTE nPills = pillsGetNumPills(&ss->sim.pb);
        BYTE i;
        for (i = 1; i <= nPills; i++) {
            pillbox pb;
            pillsGetPill(&ss->sim.pb, &pb, i);
            if (pb.inTank) continue;
            WORLD pwx = (WORLD)((int)pb.x * 256 + 128);
            WORLD pwy = (WORLD)((int)pb.y * 256 + 128);
            int pcx, pcy;
            worldTo1x(pwx, pwy, orig1xX, orig1xY, edge1xX, edge1xY, xOffset, yOffset, &pcx, &pcy);
            SDL_SetRenderDrawColor(renderer, 255, 60, 60, 128);
            SDL_FRect pr = { (float)(pcx - 8), (float)(pcy - 8), 16.0f, 16.0f };
            SDL_RenderRect(renderer, &pr);
        }
    }

    /* 3. Shell [TX, TY] world-coordinate labels (yellow text below each shell) */
    {
        shells cur = ss->sim.shs;
        while (cur) {
            if (!cur->shellDead) {
                int shx = orig1xX - 16
                        + ((int)(cur->x >> 8) - (int)xOffset) * 16
                        + ((int)(cur->x >> 4) & 0xF)
                        - edge1xX;
                int shy = orig1xY - 16
                        + ((int)(cur->y >> 8) - (int)yOffset) * 16
                        + ((int)(cur->y >> 4) & 0xF)
                        - edge1xY;
                char buf[24];
                SDL_snprintf(buf, sizeof(buf), "[%u,%u]", (unsigned)cur->x, (unsigned)cur->y);
                SDL_SetRenderDrawColor(renderer, 255, 255, 100, 220);
                SDL_RenderDebugText(renderer, (float)(shx + 2), (float)(shy + 2), buf);
            }
            cur = cur->next;
        }
    }

    /* 4. Custom 17×17 crosshair + aim line (green, 50% alpha).
     *    Center pixel (8,8) of the 17×17 image is the exact aim point.
     *    Top-left is at the same position as the old 16×16 tile sprite so
     *    center (8,8) aligns with the gunsight world position. */
    if (gs && gs->mapX != NO_GUNSIGHT) {
        int gsGameX = gs->mapX * TILE_SIZE_X + (int)gs->pixelX;
        int gsGameY = gs->mapY * TILE_SIZE_Y + (int)gs->pixelY;
        int gsX1 = orig1xX + gsGameX - TILE_SIZE_X - edge1xX;
        int gsY1 = orig1xY + gsGameY - TILE_SIZE_Y - edge1xY;

        SDL_Texture *crosshairTex = sdl3DrawGetCrosshairTex();
        if (crosshairTex) {
            SDL_FRect gsDst1x = { (float)gsX1, (float)gsY1, 17.0f, 17.0f };
            SDL_RenderTexture(renderer, crosshairTex, NULL, &gsDst1x);
        }

        /* Simulated shell trajectory — run the same HP fixed-point engine
         * as shellsUpdate for each tick and plot a 1x1 dot per tick.
         * Use playerTank (client sim) — same object tankGetGunsight uses. */
        {
            TURNTYPE angle  = tankGetAngle(playerTank);
            BYTE     len    = tankGetGunsightLength(playerTank);

            /* Starting position + trajectory: use HP fixed-point accumulation
             * matching shellsAddItem (5 start steps) + shellsUpdate per tick.
             * Use playerTank world position so we match tankGetGunsight exactly. */
            WORLD trajX = 0, trajY = 0;
            tankGetWorld(playerTank, &trajX, &trajY);
            /* Apply start offset exactly as shellsAddItem does:
             * one utilCalcDistance call, multiply by SHELL_START_ADD=5. */
            int xAdd, yAdd;
            utilCalcDistance(&xAdd, &yAdd, angle, SHELL_SPEED);
            WORLD tx = (WORLD)(trajX + 5 * xAdd);
            WORLD ty = (WORLD)(trajY + 5 * yAdd);

            /* Remaining ticks via HP accumulation, matching shellsUpdate. */
            int32_t xStepHP, yStepHP;
            utilCalcDistanceHP(&xStepHP, &yStepHP, angle, SHELL_SPEED);
            int32_t xAccHP = 0, yAccHP = 0;
            /* Parallel float position for comparison */
            float fx = (float)(int)tx;
            float fy = (float)(int)ty;
            float fvx = (float)xStepHP / 256.0f;
            float fvy = (float)yStepHP / 256.0f;

            int ticks = (SHELL_LIFE * (int)(len / 2)) - 5;

            /* Log the green path to green_path.log (overwrite each render) */
            FILE *glog = fopen("green_path.log", "w");
            if (glog) {
                fprintf(glog, "angle=%.8f len=%u ticks=%d startX=%u startY=%u startFX=%.4f startFY=%.4f\n",
                        (double)angle, (unsigned)len, ticks,
                        (unsigned)tx, (unsigned)ty, (double)fx, (double)fy);
            }

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer, 60, 240, 60, 200);

            for (int t = 0; t < ticks; t++) {
                xAccHP += xStepHP; yAccHP += yStepHP;
                tx = (WORLD)(tx + (xAccHP >> 8)); xAccHP &= 0xFF;
                ty = (WORLD)(ty + (yAccHP >> 8)); yAccHP &= 0xFF;
                fx += fvx;
                fy += fvy;

                if (glog) {
                    fprintf(glog, "t=%d tx=%u ty=%u xAcc=%d yAcc=%d fx=%.4f fy=%.4f\n",
                            t, (unsigned)tx, (unsigned)ty,
                            (int)xAccHP, (int)yAccHP, (double)fx, (double)fy);
                }

                /* Use same formula as mapViewDrawShells — no TANK_SUBTRACT */
                int px = orig1xX - 16
                       + ((int)(tx >> 8) - (int)xOffset) * 16
                       + ((int)(tx >> 4) & 0xF)
                       - edge1xX;
                int py = orig1xY - 16
                       + ((int)(ty >> 8) - (int)yOffset) * 16
                       + ((int)(ty >> 4) & 0xF)
                       - edge1xY;
                SDL_FRect dot = { (float)px, (float)py, 1.0f, 1.0f };
                SDL_RenderFillRect(renderer, &dot);
            }

            if (glog) { fclose(glog); }

        }
    }

    /* Composite the 1x overlay onto the main render target.
     * Switching back to NULL restores the game-area clip rect automatically. */
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetTextureBlendMode(gOverlayTex, SDL_BLENDMODE_BLEND);
    /* Blit 1x texture scaled up by zoomFactor: texture pixel (i,j) →
     * screen pixels (i*zoomFactor … (i+1)*zoomFactor - 1).  The game-area
     * clip rect clips anything that lands outside the visible play field. */
    SDL_FRect blitDst = { 0.0f, 0.0f,
                          (float)(texW * zoomFactor),
                          (float)(texH * zoomFactor) };
    SDL_RenderTexture(renderer, gOverlayTex, NULL, &blitDst);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

/* -------------------------------------------------------
 * Zoom window lifecycle
 * ------------------------------------------------------- */

static void zoomWindowDestroy(void) {
    if (gZoom.renderer) { SDL_DestroyRenderer(gZoom.renderer); gZoom.renderer = NULL; }
    if (gZoom.window)   { SDL_DestroyWindow(gZoom.window);     gZoom.window   = NULL; }
    gZoom.open = false;
}

static bool zoomWindowCreate(void) {
    /* Lock zoom window to the same aspect ratio as the main window so
     * tiles are never squished when the user resizes. */
    SDL_Window *mainWin = sdl3DrawGetWindow();
    if (mainWin) {
        SDL_GetWindowSize(mainWin, &gZoom.winW, &gZoom.winH);
        /* Start the zoom window at half the main window size */
        gZoom.winW /= 2;
        gZoom.winH /= 2;
        if (gZoom.winW < 200) gZoom.winW = 200;
        if (gZoom.winH < 150) gZoom.winH = 150;
    }

    gZoom.window = SDL_CreateWindow("WinBolo \xe2\x80\x94 Debug Zoom",
                                    gZoom.winW, gZoom.winH, SDL_WINDOW_RESIZABLE);
    if (!gZoom.window) return false;

    /* Enforce aspect ratio so tiles stay square when resizing */
    if (mainWin) {
        int mw = 0, mh = 0;
        SDL_GetWindowSize(mainWin, &mw, &mh);
        if (mw > 0 && mh > 0) {
            float aspect = (float)mw / (float)mh;
            SDL_SetWindowAspectRatio(gZoom.window, aspect, aspect);
        }
    }

    gZoom.renderer = SDL_CreateRenderer(gZoom.window, NULL);
    if (!gZoom.renderer) {
        SDL_DestroyWindow(gZoom.window); gZoom.window = NULL;
        return false;
    }

    gZoom.open     = true;
    gZoom.panX     = 0.0f;
    gZoom.panY     = 0.0f;
    gZoom.dragging = false;
    return true;
}

void debugZoomToggle(void) {
    if (gZoom.open) {
        zoomWindowDestroy();
    } else {
        zoomWindowCreate();
    }
}

void debugZoomCleanup(void) {
    zoomWindowDestroy();
    /* Also release the main-window overlay texture (uses the main renderer,
     * so must be freed before sdl3DrawCleanup destroys the renderer). */
    if (gOverlayTex) { SDL_DestroyTexture(gOverlayTex); gOverlayTex = NULL; }
    gOverlayTexW = gOverlayTexH = 0;
}

bool debugZoomHasFocus(void) {
    return gZoom.open && gZoom.window &&
           (SDL_GetWindowFlags(gZoom.window) & SDL_WINDOW_INPUT_FOCUS);
}

/* -------------------------------------------------------
 * Zoom window capture + render
 * ------------------------------------------------------- */

SDL_Surface *debugZoomCaptureIfOpen(SDL_Renderer *mainRenderer) {
    if (!gZoom.open) return NULL;
    return SDL_RenderReadPixels(mainRenderer, NULL);
}

void debugZoomRenderFrame(SDL_Surface *captured) {
    if (!gZoom.open || !gZoom.window || !captured) return;

    SDL_GetWindowSize(gZoom.window, &gZoom.winW, &gZoom.winH);

    int capW = captured->w;
    int capH = captured->h;

    /* Source region in the captured frame: winSize/zoom pixels, centered + panned */
    float srcW = (float)gZoom.winW / (float)gZoom.zoomFactor;
    float srcH = (float)gZoom.winH / (float)gZoom.zoomFactor;
    float srcX = (float)(capW - (int)srcW) * 0.5f + gZoom.panX;
    float srcY = (float)(capH - (int)srcH) * 0.5f + gZoom.panY;

    /* Clamp so source rect stays within the captured surface */
    if (srcX < 0.0f) srcX = 0.0f;
    if (srcY < 0.0f) srcY = 0.0f;
    if (srcW > (float)capW) srcW = (float)capW;
    if (srcH > (float)capH) srcH = (float)capH;
    if (srcX + srcW > (float)capW) srcX = (float)capW - srcW;
    if (srcY + srcH > (float)capH) srcY = (float)capH - srcH;

    /* Store for mouse-coord translation in event handler */
    gZoom.srcX = srcX;
    gZoom.srcY = srcY;
    gZoom.capW = capW;
    gZoom.capH = capH;

    SDL_Texture *tex = SDL_CreateTextureFromSurface(gZoom.renderer, captured);
    if (!tex) return;
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    /* The captured frame is already fully composited — pixels with alpha < 255
     * are the result of blending over the opaque black background.  Re-applying
     * alpha blending here causes semi-transparent sprites (bases, etc.) to be
     * double-blended and appear washed out.  BLENDMODE_NONE copies as-is. */
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);

    SDL_FRect src = { srcX, srcY, srcW, srcH };
    SDL_FRect dst = { 0.0f, 0.0f, (float)gZoom.winW, (float)gZoom.winH };

    SDL_SetRenderDrawColor(gZoom.renderer, 0, 0, 0, 255);
    SDL_RenderClear(gZoom.renderer);
    SDL_RenderTexture(gZoom.renderer, tex, &src, &dst);
    SDL_DestroyTexture(tex);
    SDL_RenderPresent(gZoom.renderer);
}

/* -------------------------------------------------------
 * Zoom window event handler
 * ------------------------------------------------------- */

/* Translate a zoom-window pixel position to the corresponding
 * main-window pixel position, using the last-rendered source rect. */
static void zoomToMain(float mx, float my, float *outX, float *outY) {
    *outX = gZoom.srcX + mx / (float)gZoom.zoomFactor;
    *outY = gZoom.srcY + my / (float)gZoom.zoomFactor;
}

bool debugZoomHandleEvent(const SDL_Event *ev) {
    if (!gZoom.open || !gZoom.window) return false;
    SDL_WindowID zoomID = SDL_GetWindowID(gZoom.window);
    SDL_Window  *mainWin = sdl3DrawGetWindow();
    SDL_WindowID mainID  = mainWin ? SDL_GetWindowID(mainWin) : 0;

    SDL_WindowID evID = 0;
    switch (ev->type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: evID = ev->window.windowID; break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:                 evID = ev->key.windowID;    break;
        case SDL_EVENT_MOUSE_WHEEL:            evID = ev->wheel.windowID;  break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:        evID = ev->button.windowID; break;
        case SDL_EVENT_MOUSE_MOTION:           evID = ev->motion.windowID; break;
        default: return false;
    }
    if (evID != zoomID) return false;

    switch (ev->type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            debugZoomToggle();
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            /* Scroll wheel zooms the debug view — not forwarded to game */
            if (ev->wheel.y > 0 && gZoom.zoomFactor < 16) gZoom.zoomFactor++;
            if (ev->wheel.y < 0 && gZoom.zoomFactor > 1)  gZoom.zoomFactor--;
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev->button.button == SDL_BUTTON_MIDDLE) {
                /* Middle-drag pans the debug view — not forwarded */
                if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    gZoom.dragging        = true;
                    gZoom.dragStartMouseX = ev->button.x;
                    gZoom.dragStartMouseY = ev->button.y;
                    gZoom.dragStartPanX   = gZoom.panX;
                    gZoom.dragStartPanY   = gZoom.panY;
                } else {
                    gZoom.dragging = false;
                }
            } else if (mainID) {
                /* Left/right click: translate coords and forward to main window */
                SDL_Event fwd = *ev;
                float mx, my;
                zoomToMain(ev->button.x, ev->button.y, &mx, &my);
                fwd.button.windowID = mainID;
                fwd.button.x = mx;
                fwd.button.y = my;
                SDL_PushEvent(&fwd);
            }
            break;

        case SDL_EVENT_MOUSE_MOTION:
            if (gZoom.dragging) {
                float dx = ev->motion.x - gZoom.dragStartMouseX;
                float dy = ev->motion.y - gZoom.dragStartMouseY;
                /* Drag right -> see content to the left -> panX decreases */
                gZoom.panX = gZoom.dragStartPanX - dx / (float)gZoom.zoomFactor;
                gZoom.panY = gZoom.dragStartPanY - dy / (float)gZoom.zoomFactor;
            }
            /* Always forward motion to main — moves the gunsight */
            if (mainID) {
                SDL_Event fwd = *ev;
                float mx, my;
                zoomToMain(ev->motion.x, ev->motion.y, &mx, &my);
                fwd.motion.windowID = mainID;
                fwd.motion.x = mx;
                fwd.motion.y = my;
                SDL_PushEvent(&fwd);
            }
            break;

        case SDL_EVENT_KEY_DOWN:
            if (ev->key.scancode == SDL_SCANCODE_R && !ev->key.repeat) {
                /* R resets pan in the debug view */
                gZoom.panX     = 0.0f;
                gZoom.panY     = 0.0f;
                gZoom.dragging = false;
            }
            /* Forward all keys to main window (tank movement, fire, etc.) */
            if (mainID) {
                SDL_Event fwd = *ev;
                fwd.key.windowID = mainID;
                SDL_PushEvent(&fwd);
            }
            break;

        case SDL_EVENT_KEY_UP:
            /* Forward key-up so held keys (movement, turret) release properly */
            if (mainID) {
                SDL_Event fwd = *ev;
                fwd.key.windowID = mainID;
                SDL_PushEvent(&fwd);
            }
            break;
    }
    return true;
}
