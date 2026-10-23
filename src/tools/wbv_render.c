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
 * Name:          wbv_render.c
 * Purpose:
 *   WbvRender: draws a round log (.wbv) to video frames with
 *   no window, so ffmpeg can make clips of a game.
 *
 *   WbvRender <in.wbv> -out <dir | -> [options]
 *
 *     -out <dir>     write <dir>/frame_000000.png, frame_000001.png, ...
 *     -out -         write raw rgb24 frames to stdout, for an ffmpeg pipe
 *     -from <sec>    start this far into the game (default 0)
 *     -to <sec>      stop here (default: the end of the log)
 *     -fps <n>       frames per second of replay time (default 50)
 *     -view x y w h  a fixed view: top-left map square and size in squares
 *     -follow <p>    follow a player, by name or by player number
 *     -size w h      the view size in squares when not -view (default 40 30)
 *
 *   With neither -view nor -follow the view follows the lowest-numbered
 *   player who has a tank.
 *     -self <name>   draw the round from this player's side
 *     -scale <n>     make each pixel n by n (default 1)
 *     -quiet         no progress lines on stderr
 *
 *   Times are seconds ("75", "75.5" or "1:15.5") from the start of the
 *   game; the lobby before it is not shown. One log entry is one tick,
 *   20 ms of play, and frame n is the tick n * 50 / fps after the first
 *   frame, so the frames are at exact replay times whatever the speed of
 *   the machine.
 *
 *   ffmpeg, with the frames on a pipe (the size is printed on stderr):
 *     WbvRender round.wbv -out - -from 60 -to 70 -follow Bob |
 *       ffmpeg -f rawvideo -pixel_format rgb24 -video_size 640x480
 *              -framerate 50 -i - -c:v libx264 -pix_fmt yuv420p
 *              -crf 20 clip.mp4
 *   or from PNG frames:
 *     ffmpeg -framerate 50 -i out/frame_%06d.png -c:v libx264
 *            -pix_fmt yuv420p -crf 20 clip.mp4
 *
 *   The drawing is the log viewer's own: this is a host for the
 *   embedded reel (lv_embed.c), the same one the lobby's recap and its
 *   GIF export use. SDL runs on its "offscreen" video driver and the
 *   reel draws with a software renderer into memory, so no window ever
 *   opens. The sprite and tile files load from the data folder beside
 *   the program, as the viewer's do.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#endif

#include "backend.h"
#include "logviewer.h"
#include "lv_host.h"
#include "lv_players.h"
#include "draw.h"
#include "clientmutex.h"
#include "tiles.h"
#include "wbv_render_args.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "../third_party/stb/stb_image_write.h"

/* The place of 1.0 in draw.c's zoom table. The reel opens at the widest step,
 * 0.5; the frames are drawn at 1x and scaled here if -scale asks. */
#define WBV_RENDER_ZOOM_STEP_1X 5

static void printUsage(void) {
    fprintf(stderr,
        "Usage: WbvRender <in.wbv> -out <dir | -> [options]\n"
        "  -out <dir>     write <dir>/frame_%%06d.png\n"
        "  -out -         write raw rgb24 frames to stdout\n"
        "  -from <sec>    start this far into the game (default 0)\n"
        "  -to <sec>      stop here (default: the end of the log)\n"
        "  -fps <n>       frames per second of replay time (default 50)\n"
        "  -view x y w h  fixed view: top-left map square, size in squares\n"
        "  -follow <p>    follow a player, by name or player number\n"
        "  -size w h      view size in squares when not -view (default %d %d)\n"
        "  -self <name>   draw the round from this player's side\n"
        "  -scale <n>     make each pixel n by n (default 1)\n"
        "  -quiet         no progress lines on stderr\n"
        "With neither -view nor -follow the view follows the lowest-numbered\n"
        "player who has a tank.\n",
        WBV_RENDER_DEFAULT_W, WBV_RENDER_DEFAULT_H);
}

/* Reads the whole file into a malloc'd buffer. */
static uint8_t *readFile(const char *path, size_t *outLen) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    Sint64 size;
    uint8_t *buf;

    if (io == NULL) {
        return NULL;
    }
    size = SDL_GetIOSize(io);
    if (size <= 0) {
        SDL_CloseIO(io);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL) {
        SDL_CloseIO(io);
        return NULL;
    }
    if (SDL_ReadIO(io, buf, (size_t)size) != (size_t)size) {
        free(buf);
        SDL_CloseIO(io);
        return NULL;
    }
    SDL_CloseIO(io);
    *outLen = (size_t)size;
    return buf;
}

static bool makeDir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return (st.st_mode & S_IFDIR) != 0;
    }
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

/* Writes the players in the log at this moment, so a -follow name or number
 * can be picked from them. */
static void listPlayers(void) {
    BYTE i;
    char name[PLAYER_NAME_LEN + 1];
    bool any = false;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!lv_playersIsInUse(i)) {
            continue;
        }
        lv_playersGetPlayerName(i, name, sizeof(name));
        fprintf(stderr, "  player %u: %s\n", (unsigned)i, name);
        any = true;
    }
    if (!any) {
        fprintf(stderr, "  (no players yet)\n");
    }
}

/* The name -follow means: the text itself, or the name of that player number
 * when it is all digits and that player is in the game. */
static void followName(const char *follow, char *dest, size_t destLen) {
    const char *p = follow;
    int n = 0;

    snprintf(dest, destLen, "%s", follow);
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n >= MAX_TANKS) {
            return;
        }
        p++;
    }
    if (*p != '\0' || p == follow) {
        return;
    }
    if (lv_playersIsInUse((BYTE)n)) {
        lv_playersGetPlayerName((BYTE)n, dest, destLen);
    }
}

/* With no -view and no -follow: follow the lowest-numbered player who has a
 * tank on the map. Writes their name into dest. */
static bool followFirstTank(char *dest, size_t destLen) {
    BYTE i;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!lv_playersIsInUse(i)) {
            continue;
        }
        lv_playersGetPlayerName(i, dest, destLen);
        if (lvEmbedFocusPlayerByName(dest)) {
            return true;
        }
    }
    return false;
}

/* Puts the view's top-left at map square (x, y) and stops the camera
 * following anyone, under the lock, as lvEmbedPanDelta does. */
static void setFixedView(int x, int y) {
    LogViewerState *lv = lv_screenGetState();
    lv_clientMutexWaitFor();
    lv->centredTank = FALSE;
    lv_drawDirtyScreen();
    lv_screenPanToTotalPixels(x * TILE_SIZE_X, y * TILE_SIZE_Y);
    lv_clientMutexRelease();
    lv_windowNeedRedraw();
}

/* Copies the visible slice of the reel's texture into rgb (w * scale by
 * h * scale, three bytes a pixel). */
static bool readFrame(SDL_Renderer *renderer, int scale, uint8_t *rgb,
                      int outW, int outH) {
    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    SDL_Rect rect;
    SDL_Surface *raw, *conv;
    int x, y, row;

    if (!lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) ||
        tex == NULL) {
        return false;
    }
    rect.x = srcX;
    rect.y = srcY;
    rect.w = outW / scale;
    rect.h = outH / scale;
    if (rect.x + rect.w > texW) rect.x = texW - rect.w;
    if (rect.y + rect.h > texH) rect.y = texH - rect.h;
    if (rect.x < 0 || rect.y < 0) {
        return false;
    }
    if (!SDL_SetRenderTarget(renderer, (SDL_Texture *)tex)) {
        return false;
    }
    raw = SDL_RenderReadPixels(renderer, &rect);
    SDL_SetRenderTarget(renderer, NULL);
    if (raw == NULL) {
        return false;
    }
    conv = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGB24);
    SDL_DestroySurface(raw);
    if (conv == NULL) {
        return false;
    }
    for (y = 0; y < rect.h; y++) {
        const uint8_t *src = (const uint8_t *)conv->pixels + (size_t)y * (size_t)conv->pitch;
        uint8_t *dst = rgb + (size_t)(y * scale) * (size_t)outW * 3u;
        if (scale == 1) {
            memcpy(dst, src, (size_t)rect.w * 3u);
            continue;
        }
        for (x = 0; x < rect.w; x++) {
            int k;
            for (k = 0; k < scale; k++) {
                uint8_t *d = dst + ((size_t)x * (size_t)scale + (size_t)k) * 3u;
                d[0] = src[x * 3 + 0];
                d[1] = src[x * 3 + 1];
                d[2] = src[x * 3 + 2];
            }
        }
        for (row = 1; row < scale; row++) {
            memcpy(dst + (size_t)row * (size_t)outW * 3u, dst, (size_t)outW * 3u);
        }
    }
    SDL_DestroySurface(conv);
    return true;
}

int main(int argc, char **argv) {
    WbvRenderArgs args;
    char err[256];
    uint8_t *zip;
    size_t zipLen = 0;
    SDL_Window *window = NULL;
    SDL_Surface *canvas = NULL;
    SDL_Renderer *renderer = NULL;
    const char *driver;
    int viewPxW, viewPxH, outW, outH;
    uint8_t *rgb = NULL;
    uint32_t startMs = 0, totalMs = 0, endMs, frames, frame, ticksDone = 0;
    char followTarget[PLAYER_NAME_LEN + 1];
    bool following = false;
    int rc = 1;

    if (!wbvRenderParseArgs(argc, argv, &args, err, sizeof(err))) {
        if (err[0] != '\0') {
            fprintf(stderr, "WbvRender: %s\n\n", err);
        }
        printUsage();
        return err[0] != '\0' ? 2 : 0;
    }

    /* The offscreen driver, or nothing: this program must never open a
     * window, so a build of SDL without it is a failure, not a reason to
     * fall back to a real one. */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "WbvRender: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    driver = SDL_GetCurrentVideoDriver();
    if (driver == NULL || strcmp(driver, "offscreen") != 0) {
        fprintf(stderr, "WbvRender: the offscreen video driver is not available "
                        "(got %s)\n", driver != NULL ? driver : "none");
        SDL_Quit();
        return 1;
    }
    if (!TTF_Init()) {
        fprintf(stderr, "WbvRender: TTF_Init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    viewPxW = args.viewW * TILE_SIZE_X;
    viewPxH = args.viewH * TILE_SIZE_Y;
    outW = viewPxW * args.scale;
    outH = viewPxH * args.scale;

    /* The reel wants a window to borrow. On the offscreen driver it is only
     * a record in memory. */
    window = SDL_CreateWindow("WbvRender", viewPxW, viewPxH, SDL_WINDOW_HIDDEN);
    canvas = SDL_CreateSurface(viewPxW, viewPxH, SDL_PIXELFORMAT_ARGB8888);
    if (window == NULL || canvas == NULL) {
        fprintf(stderr, "WbvRender: could not make the offscreen window: %s\n",
                SDL_GetError());
        goto done;
    }
    renderer = SDL_CreateSoftwareRenderer(canvas);
    if (renderer == NULL) {
        fprintf(stderr, "WbvRender: could not make the software renderer: %s\n",
                SDL_GetError());
        goto done;
    }

    zip = readFile(args.inPath, &zipLen);
    if (zip == NULL) {
        fprintf(stderr, "WbvRender: could not read %s\n", args.inPath);
        goto done;
    }

    /* This program steps the decoder itself, so the reel starts no timer
     * that would play ticks by the wall clock. */
    lvEmbedSetErrorsToStderr(true);
    lv_windowSetHostRunsTicks(true);
    if (!lvEmbedBegin(window, renderer, zip, zipLen, viewPxW, viewPxH)) {
        fprintf(stderr, "WbvRender: %s is not a log the viewer can load "
                        "(or the data folder beside WbvRender is missing)\n",
                args.inPath);
        goto done;
    }
    lvEmbedPause();
    lvEmbedSetTankLabelsInTexture(true);
    lv_clientMutexWaitFor();
    lv_drawSetZoomStep(WBV_RENDER_ZOOM_STEP_1X, viewPxW / 2, viewPxH / 2);
    lv_clientMutexRelease();
    lvEmbedSetViewportSize(viewPxW, viewPxH);
    if (lv_screenGetSizeX() != args.viewW || lv_screenGetSizeY() != args.viewH) {
        fprintf(stderr, "WbvRender: the view is %ux%u squares, not %dx%d\n",
                (unsigned)lv_screenGetSizeX(), (unsigned)lv_screenGetSizeY(),
                args.viewW, args.viewH);
        goto done;
    }
    if (args.self != NULL) {
        lvEmbedSetSelfName(args.self);
    }

    lvEmbedSeekToTime(args.fromMs);
    lvEmbedGetProgress(&startMs, &totalMs);
    endMs = totalMs;
    if (args.haveTo && args.toMs < endMs) {
        endMs = args.toMs;
    }
    if (startMs >= endMs) {
        fprintf(stderr, "WbvRender: the game is %u.%03u s long; nothing to draw "
                        "from %u.%03u s\n",
                totalMs / 1000u, totalMs % 1000u, startMs / 1000u, startMs % 1000u);
        goto done;
    }
    frames = wbvRenderFrameCount(endMs - startMs, args.fps);

    if (!args.quiet) {
        fprintf(stderr, "WbvRender: %s, game %u.%03u s; frames %u.%03u s to "
                        "%u.%03u s, %u frames at %d fps, %dx%d pixels\n",
                args.inPath, totalMs / 1000u, totalMs % 1000u,
                startMs / 1000u, startMs % 1000u, endMs / 1000u, endMs % 1000u,
                frames, args.fps, outW, outH);
        fprintf(stderr, "WbvRender: players at the first frame:\n");
        listPlayers();
    }

    if (args.haveView) {
        setFixedView(args.viewX, args.viewY);
    }
    if (args.follow != NULL) {
        followName(args.follow, followTarget, sizeof(followTarget));
    }

    if (args.rawOut) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    } else if (!makeDir(args.outDir)) {
        fprintf(stderr, "WbvRender: could not make the folder %s: %s\n",
                args.outDir, strerror(errno));
        goto done;
    }

    rgb = (uint8_t *)malloc((size_t)outW * (size_t)outH * 3u);
    if (rgb == NULL) {
        fprintf(stderr, "WbvRender: out of memory\n");
        goto done;
    }

    for (frame = 0; frame < frames; frame++) {
        uint32_t want = wbvRenderFrameTicks(frame, args.fps);
        if (want > ticksDone) {
            lvEmbedStepTicks((int)(want - ticksDone));
            ticksDone = want;
        }
        /* A player who has not joined yet, or is dead, has no tank to
         * follow; keep asking until they have one. After that the camera
         * stays with them on its own. */
        if (!args.haveView && !following) {
            if (args.follow != NULL) {
                following = lvEmbedFocusPlayerByName(followTarget);
            } else {
                following = followFirstTank(followTarget, sizeof(followTarget));
            }
            if (following && !args.quiet) {
                fprintf(stderr, "WbvRender: following %s from frame %u\n",
                        followTarget, frame);
            }
        }
        lv_windowNeedRedraw();
        if (!readFrame(renderer, args.scale, rgb, outW, outH)) {
            fprintf(stderr, "WbvRender: could not read frame %u: %s\n", frame,
                    SDL_GetError());
            goto done;
        }
        if (args.rawOut) {
            size_t len = (size_t)outW * (size_t)outH * 3u;
            if (fwrite(rgb, 1, len, stdout) != len) {
                /* The reader went away (ffmpeg stopped): not an error. */
                break;
            }
        } else {
            char path[1024];
            snprintf(path, sizeof(path), "%s/frame_%06u.png", args.outDir, frame);
            if (!stbi_write_png(path, outW, outH, 3, rgb, outW * 3)) {
                fprintf(stderr, "WbvRender: could not write %s\n", path);
                goto done;
            }
        }
        if (!args.quiet && (frame % 250u) == 0u && frame > 0u) {
            fprintf(stderr, "WbvRender: frame %u of %u\n", frame, frames);
        }
    }
    if (args.follow != NULL && !following) {
        fprintf(stderr, "WbvRender: never found a tank for '%s' to follow; "
                        "the players are:\n", args.follow);
        listPlayers();
    }
    if (args.rawOut) {
        fflush(stdout);
    }
    if (!args.quiet) {
        fprintf(stderr, "WbvRender: wrote %u frames\n", frame);
    }
    rc = 0;

done:
    free(rgb);
    lvEmbedEnd();
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (canvas != NULL) SDL_DestroySurface(canvas);
    if (window != NULL) SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return rc;
}
