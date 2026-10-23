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
 * Name:          wbv_render_args.h
 * Purpose:
 *   The command line of WbvRender and the arithmetic that
 *   maps an output frame to a replay tick. Plain C with no
 *   SDL, so the unit tests link it on its own.
 *********************************************************/

#ifndef WBV_RENDER_ARGS_H
#define WBV_RENDER_ARGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One log entry is one game tick, 20 ms of play. */
#define WBV_RENDER_TICK_MS        20u
#define WBV_RENDER_TICKS_PER_SEC  50u

/* What the view shows when neither -view nor -follow is given, and the size
 * -follow uses unless -size says otherwise, in map squares. */
#define WBV_RENDER_DEFAULT_W      40
#define WBV_RENDER_DEFAULT_H      30
/* The tile grid holds at most this many squares on a side. */
#define WBV_RENDER_MAX_SQUARES    255
#define WBV_RENDER_MAX_SCALE      8
#define WBV_RENDER_MAX_FPS        200

typedef struct {
    const char *inPath;     /* the .wbv to read */
    const char *outDir;     /* folder for frame_%06d.png; NULL with rawOut */
    bool        rawOut;     /* "-out -": raw rgb24 frames on stdout */
    uint32_t    fromMs;     /* start, from the start of the game */
    bool        haveTo;     /* false: run to the end of the log */
    uint32_t    toMs;       /* end, from the start of the game */
    int         fps;        /* output frames per second of replay time */
    bool        haveView;   /* a fixed view was given with -view */
    int         viewX;      /* top-left map square of the fixed view */
    int         viewY;
    int         viewW;      /* view size in map squares */
    int         viewH;
    const char *follow;     /* player name or number to follow; NULL for none */
    const char *self;       /* player whose side the round is drawn from */
    int         scale;      /* whole-number pixel scale of the output */
    bool        quiet;      /* no progress lines on stderr */
} WbvRenderArgs;

/* Fills out from argv (argv[0] is the program). Returns true on success. On
 * failure writes a one-line reason into err and returns false. "-h" and
 * "-help" also return false, with an empty err. */
bool wbvRenderParseArgs(int argc, char **argv, WbvRenderArgs *out,
                        char *err, size_t errLen);

/* The number of ticks from the first frame to frame number frame, at fps
 * frames per second of replay time. Frame n shows the moment n / fps seconds
 * after the first frame, rounded down to a whole tick, so every frame is at an
 * exact replay time and none is stepped by the wall clock. */
uint32_t wbvRenderFrameTicks(uint32_t frame, int fps);

/* How many frames cover durationMs of replay at fps: the frames whose time is
 * before the end. */
uint32_t wbvRenderFrameCount(uint32_t durationMs, int fps);

/* Parses a time in seconds ("12", "12.5", "1:05", "1:05.25") into whole
 * milliseconds. Returns false for anything else or a negative time. */
bool wbvRenderParseSeconds(const char *text, uint32_t *outMs);

#endif /* WBV_RENDER_ARGS_H */
