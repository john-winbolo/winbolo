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
#include "../../server/server_sim.h"
#include "../gamefront.h"
#include <math.h>

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
 * Coordinate helpers (main window only)
 * ------------------------------------------------------- */

/* Convert world coord (256 units/tile) to screen pixel in the main window.
 *
 * Replicates screentank.c tankGetScreenMX/PX (TANK_SUBTRACT = 128) and the
 * mapViewDrawTanks formula so the box lands exactly on the visual sprite:
 *   sprite top-left = originX - tileW + bbx*zoom - edgeX
 *   sprite center   = top-left + tileW/2  (tileW = 16*zoom, half = 8*zoom)
 */
static void worldToMain(WORLD wx, WORLD wy,
                        int originX, int originY, int edgeX, int edgeY,
                        int zoomFactor, BYTE xOffset, BYTE yOffset,
                        float *cx, float *cy) {
    int dx  = (int)wx - 128;          /* TANK_SUBTRACT = 128 */
    int dy  = (int)wy - 128;
    int mx  = dx >> 8;                /* abs tile of sprite top-left */
    int px  = (dx >> 4) & 0xF;       /* pixel within tile (0-15) */
    int my  = dy >> 8;
    int py  = (dy >> 4) & 0xF;
    int bbx = (mx - (int)xOffset) * 16 + px;
    int bby = (my - (int)yOffset) * 16 + py;
    *cx = (float)(originX - 8 * zoomFactor + bbx * zoomFactor - edgeX);
    *cy = (float)(originY - 8 * zoomFactor + bby * zoomFactor - edgeY);
}

/* -------------------------------------------------------
 * Drawing helpers
 * ------------------------------------------------------- */

/* Draw a line with width lineW screen pixels using a filled quad
 * (two triangles via SDL_RenderGeometry). Handles any angle.
 * Color components are 0-255 uint8. */
static void renderThickLine(SDL_Renderer *renderer,
                            float x1, float y1, float x2, float y2,
                            float lineW,
                            Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    float hw = lineW * 0.5f;
    float nx = -dy / len * hw;
    float ny =  dx / len * hw;
    SDL_FColor fc;
    fc.r = (float)r / 255.0f;
    fc.g = (float)g / 255.0f;
    fc.b = (float)b / 255.0f;
    fc.a = (float)a / 255.0f;
    SDL_Vertex v[4];
    v[0].position.x = x1 + nx; v[0].position.y = y1 + ny; v[0].color = fc; v[0].tex_coord.x = 0; v[0].tex_coord.y = 0;
    v[1].position.x = x2 + nx; v[1].position.y = y2 + ny; v[1].color = fc; v[1].tex_coord.x = 0; v[1].tex_coord.y = 0;
    v[2].position.x = x2 - nx; v[2].position.y = y2 - ny; v[2].color = fc; v[2].tex_coord.x = 0; v[2].tex_coord.y = 0;
    v[3].position.x = x1 - nx; v[3].position.y = y1 - ny; v[3].color = fc; v[3].tex_coord.x = 0; v[3].tex_coord.y = 0;
    int idx[6] = { 0, 1, 2,  0, 2, 3 };
    SDL_RenderGeometry(renderer, NULL, v, 4, idx, 6);
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
                          BYTE playerNum, screenGunsight *gs) {
    ServerSim *ss = gameFrontGetServerSim();
    if (!ss) return;

    float zf = (float)zoomFactor;

    /* All overlays at 50% alpha, blended over the game image */
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* 1. Tank bounding box (yellow, 50% alpha)
     *
     * Hitbox check: abs(tank.x - shell.x) < 128  (WORLD integers, strict <)
     * Farthest INCLUDED shell: world offset ±127 = ±7 game pixels = ±7*zf px.
     * Drawing at ±8*zf (the excluded boundary) would put border pixels outside
     * the hitbox.  Use ±7*zf so the inside edge of the border is the
     * outermost included game pixel column. */
    WORLD twx = 0, twy = 0;
    tankGetWorld(&ss->sim.tanks[playerNum], &twx, &twy);
    float cx, cy;
    worldToMain(twx, twy, originX, originY, edgeX, edgeY,
                zoomFactor, xOffset, yOffset, &cx, &cy);

    {
        float half = 7.0f * zf;
        SDL_SetRenderDrawColor(renderer, 255, 220, 0, 128);
        SDL_FRect r = { cx - half, cy - half, half * 2.0f, half * 2.0f };
        SDL_RenderRect(renderer, &r);
        /* Center dot: one game pixel */
        SDL_FRect dot = { cx - zf * 0.5f, cy - zf * 0.5f, zf, zf };
        SDL_RenderFillRect(renderer, &dot);
    }

    /* 2. Pillbox tile boxes (red, 50% alpha)
     *
     * Hitbox check: shell.x >> 8 == pill.x  (tile-index equality)
     * Tile covers world [pill.x*256, pill.x*256+255] — all four edges
     * of the current rect fall within the included range, so the box is
     * already showing the correct inside edge. */
    {
        BYTE nPills = pillsGetNumPills(&ss->sim.pb);
        BYTE i;
        for (i = 1; i <= nPills; i++) {
            pillbox pb;
            pillsGetPill(&ss->sim.pb, &pb, i);
            if (pb.inTank) continue;
            WORLD pwx = (WORLD)((int)pb.x * 256 + 128);
            WORLD pwy = (WORLD)((int)pb.y * 256 + 128);
            float pcx, pcy;
            worldToMain(pwx, pwy, originX, originY, edgeX, edgeY,
                        zoomFactor, xOffset, yOffset, &pcx, &pcy);
            float half = 8.0f * zf;
            SDL_SetRenderDrawColor(renderer, 255, 60, 60, 128);
            SDL_FRect pr = { pcx - half, pcy - half, half * 2.0f, half * 2.0f };
            SDL_RenderRect(renderer, &pr);
        }
    }

    /* 3. Gunsight sprite + aim line (green, 1 game pixel wide, 50% alpha)
     *    Drawn before shell crosshairs so shells render on top. */
    if (gs && gs->mapX != NO_GUNSIGHT) {
        float gsInset = 0.05f;
        SDL_FRect gsSrc = {
            (float)(GUNSIGHT_X * sheetScale) + gsInset,
            (float)(GUNSIGHT_Y * sheetScale) + gsInset,
            (float)(TILE_SIZE_X * sheetScale) - 2.0f * gsInset,
            (float)(TILE_SIZE_Y * sheetScale) - 2.0f * gsInset
        };
        int gsGameX = gs->mapX * TILE_SIZE_X + (int)gs->pixelX;
        int gsGameY = gs->mapY * TILE_SIZE_Y + (int)gs->pixelY;
        SDL_FRect gsDst = {
            (float)(originX + (gsGameX - TILE_SIZE_X) * zoomFactor - edgeX),
            (float)(originY + (gsGameY - TILE_SIZE_Y) * zoomFactor - edgeY),
            (float)tileW, (float)tileH
        };
        SDL_RenderTexture(renderer, tilesTex, &gsSrc, &gsDst);

        /* Aim line: 1 game pixel (zoomFactor screen pixels) wide */
        float gsCx = gsDst.x + (float)tileW * 0.5f;
        float gsCy = gsDst.y + (float)tileH * 0.5f;
        renderThickLine(renderer, cx, cy, gsCx, gsCy, zf, 60, 240, 60, 128);
    }

    /* 4. Shell crosshairs (blue, 50% alpha, 1 game pixel thick arms)
     *    Drawn last so they sit on top of everything including the aim line. */
    {
        shells cur = ss->sim.shs;
        while (cur) {
            if (!cur->shellDead) {
                float scx, scy;
                worldToMain(cur->x, cur->y, originX, originY, edgeX, edgeY,
                            zoomFactor, xOffset, yOffset, &scx, &scy);
                /* Crosshair arms: 1 game pixel thick, ±4 game pixels long.
                 * Arms are axis-aligned so draw as filled rects (grid-exact). */
                float arm = 4.0f * zf;
                SDL_SetRenderDrawColor(renderer, 80, 180, 255, 128);
                SDL_FRect hArm = { scx - arm, scy, arm * 2.0f, zf };
                SDL_FRect vArm = { scx, scy - arm, zf, arm * 2.0f };
                SDL_RenderFillRect(renderer, &hArm);
                SDL_RenderFillRect(renderer, &vArm);
            }
            cur = cur->next;
        }
    }

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
