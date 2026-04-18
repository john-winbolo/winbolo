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
#include "../tiles.h"
#include "../../bolo/global.h"
#include "../../bolo/tank.h"
#include "../../bolo/pillbox.h"
#include "../../bolo/shells.h"
#include "../../server/server_sim.h"
#include "../gamefront.h"

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

    float half = (float)(tileW / 2);

    /* 1. Tank bounding box (yellow) */
    WORLD twx = 0, twy = 0;
    tankGetWorld(&ss->sim.tanks[playerNum], &twx, &twy);
    float cx, cy;
    worldToMain(twx, twy, originX, originY, edgeX, edgeY,
                zoomFactor, xOffset, yOffset, &cx, &cy);

    SDL_SetRenderDrawColor(renderer, 255, 220, 0, 255);
    SDL_FRect r = { cx - half, cy - half, half * 2.0f, half * 2.0f };
    SDL_RenderRect(renderer, &r);
    SDL_FRect dot = { cx - 2.0f, cy - 2.0f, 5.0f, 5.0f };
    SDL_RenderFillRect(renderer, &dot);

    /* 2. Pillbox tile boxes (red, skip inTank) */
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
            SDL_SetRenderDrawColor(renderer, 255, 60, 60, 200);
            SDL_FRect pr = { pcx - half, pcy - half, (float)tileW, (float)tileH };
            SDL_RenderRect(renderer, &pr);
        }
    }

    /* 3. Shells: center dot + hit-check box (blue) */
    {
        shells cur = ss->sim.shs;
        while (cur) {
            if (!cur->shellDead) {
                float scx, scy;
                worldToMain(cur->x, cur->y, originX, originY, edgeX, edgeY,
                            zoomFactor, xOffset, yOffset, &scx, &scy);
                SDL_SetRenderDrawColor(renderer, 80, 180, 255, 255);
                SDL_FRect sdot = { scx - 3.0f, scy - 3.0f, 7.0f, 7.0f };
                SDL_RenderFillRect(renderer, &sdot);
                SDL_FRect sr = { scx - half, scy - half, half * 2.0f, half * 2.0f };
                SDL_RenderRect(renderer, &sr);
            }
            cur = cur->next;
        }
    }

    /* 4. Gunsight sprite (exact copy of sdl3draw.c logic) + 5. Aim line */
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

        /* Aim line: tank center -> gunsight center */
        float gsCx = gsDst.x + (float)tileW  * 0.5f;
        float gsCy = gsDst.y + (float)tileH * 0.5f;
        SDL_SetRenderDrawColor(renderer, 60, 240, 60, 200);
        SDL_RenderLine(renderer, cx, cy, gsCx, gsCy);
    }
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
    gZoom.window = SDL_CreateWindow("WinBolo \xe2\x80\x94 Debug Zoom",
                                    gZoom.winW, gZoom.winH, SDL_WINDOW_RESIZABLE);
    if (!gZoom.window) return false;

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

    SDL_Texture *tex = SDL_CreateTextureFromSurface(gZoom.renderer, captured);
    if (!tex) return;
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

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

bool debugZoomHandleEvent(const SDL_Event *ev) {
    if (!gZoom.open || !gZoom.window) return false;
    SDL_WindowID zoomID = SDL_GetWindowID(gZoom.window);

    SDL_WindowID evID = 0;
    switch (ev->type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: evID = ev->window.windowID; break;
        case SDL_EVENT_KEY_DOWN:               evID = ev->key.windowID;    break;
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
            if (ev->wheel.y > 0 && gZoom.zoomFactor < 16) gZoom.zoomFactor++;
            if (ev->wheel.y < 0 && gZoom.zoomFactor > 1)  gZoom.zoomFactor--;
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (ev->button.button == SDL_BUTTON_MIDDLE) {
                gZoom.dragging       = true;
                gZoom.dragStartMouseX = ev->button.x;
                gZoom.dragStartMouseY = ev->button.y;
                gZoom.dragStartPanX   = gZoom.panX;
                gZoom.dragStartPanY   = gZoom.panY;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (ev->button.button == SDL_BUTTON_MIDDLE) gZoom.dragging = false;
            break;

        case SDL_EVENT_MOUSE_MOTION:
            if (gZoom.dragging) {
                float dx = ev->motion.x - gZoom.dragStartMouseX;
                float dy = ev->motion.y - gZoom.dragStartMouseY;
                /* Drag right -> see content to the left -> panX decreases */
                gZoom.panX = gZoom.dragStartPanX - dx / (float)gZoom.zoomFactor;
                gZoom.panY = gZoom.dragStartPanY - dy / (float)gZoom.zoomFactor;
            }
            break;

        case SDL_EVENT_KEY_DOWN:
            if (ev->key.scancode == SDL_SCANCODE_R) {
                gZoom.panX     = 0.0f;
                gZoom.panY     = 0.0f;
                gZoom.dragging = false;
            }
            break;
    }
    return true;
}
