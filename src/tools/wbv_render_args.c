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
 * Name:          wbv_render_args.c
 * Purpose:
 *   WbvRender's command line and frame timing. See
 *   wbv_render_args.h.
 *********************************************************/

#include "wbv_render_args.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reads a whole number of at most nine digits from *p and moves *p past it.
 * False when there is no digit. */
static bool readDigits(const char **p, uint32_t *outValue, int *outDigits) {
    uint32_t value = 0;
    int digits = 0;

    while (**p >= '0' && **p <= '9') {
        if (digits >= 9) {
            return false;
        }
        value = value * 10u + (uint32_t)(**p - '0');
        digits++;
        (*p)++;
    }
    if (digits == 0) {
        return false;
    }
    *outValue = value;
    if (outDigits != NULL) {
        *outDigits = digits;
    }
    return true;
}

bool wbvRenderParseSeconds(const char *text, uint32_t *outMs) {
    const char *p = text;
    uint32_t minutes = 0, seconds = 0, fracMs = 0;
    uint64_t total;

    if (text == NULL || outMs == NULL) {
        return false;
    }
    if (!readDigits(&p, &seconds, NULL)) {
        return false;
    }
    if (*p == ':') {
        /* "m:ss": what came first was minutes. */
        p++;
        minutes = seconds;
        if (!readDigits(&p, &seconds, NULL) || seconds >= 60) {
            return false;
        }
    }
    if (*p == '.') {
        uint32_t frac = 0;
        int digits = 0;
        p++;
        if (!readDigits(&p, &frac, &digits)) {
            return false;
        }
        /* Keep the first three places, which are the milliseconds. */
        while (digits > 3) {
            frac /= 10u;
            digits--;
        }
        while (digits < 3) {
            frac *= 10u;
            digits++;
        }
        fracMs = frac;
    }
    if (*p != '\0') {
        return false;
    }
    total = (uint64_t)minutes * 60000u + (uint64_t)seconds * 1000u + fracMs;
    if (total > 0xFFFFFFFFu) {
        return false;
    }
    *outMs = (uint32_t)total;
    return true;
}

int wbvRenderFollowNumber(const char *follow, int maxPlayers) {
    const char *p = follow;
    int n = 0;

    if (follow == NULL || follow[0] == '\0') {
        return -1;
    }
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n >= maxPlayers) {
            return -1;
        }
        p++;
    }
    return *p == '\0' ? n : -1;
}

uint32_t wbvRenderFrameTicks(uint32_t frame, int fps) {
    if (fps <= 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)frame * WBV_RENDER_TICKS_PER_SEC) / (uint64_t)fps);
}

uint32_t wbvRenderFrameCount(uint32_t durationMs, int fps) {
    if (fps <= 0) {
        return 0;
    }
    /* Frame n is at n * 1000 / fps ms; count the n for which that is below
     * durationMs. */
    return (uint32_t)(((uint64_t)durationMs * (uint64_t)fps + 999u) / 1000u);
}

/* Reads a whole number in [lo, hi]. */
static bool parseInt(const char *text, int lo, int hi, int *out) {
    char *end = NULL;
    long v;

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    v = strtol(text, &end, 10);
    if (end == NULL || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool wbvRenderParseArgs(int argc, char **argv, WbvRenderArgs *out,
                        char *err, size_t errLen) {
    int i;
    bool haveSize = false;
    int sizeW = WBV_RENDER_DEFAULT_W, sizeH = WBV_RENDER_DEFAULT_H;

    if (err != NULL && errLen > 0) {
        err[0] = '\0';
    }
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->fps = 50;
    out->scale = 1;

#define ARG_FAIL(...)                                    \
    do {                                                 \
        if (err != NULL && errLen > 0) {                 \
            snprintf(err, errLen, __VA_ARGS__);          \
        }                                                \
        return false;                                    \
    } while (0)
#define NEED(n)                                                       \
    do {                                                              \
        if (i + (n) >= argc) {                                        \
            ARG_FAIL("%s needs %d value%s", a, (n), (n) == 1 ? "" : "s"); \
        }                                                             \
    } while (0)

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "-help") == 0 ||
            strcmp(a, "--help") == 0) {
            return false;
        } else if (strcmp(a, "-out") == 0) {
            NEED(1);
            i++;
            if (strcmp(argv[i], "-") == 0) {
                out->rawOut = true;
                out->outDir = NULL;
            } else {
                out->rawOut = false;
                out->outDir = argv[i];
            }
        } else if (strcmp(a, "-from") == 0) {
            NEED(1);
            if (!wbvRenderParseSeconds(argv[++i], &out->fromMs)) {
                ARG_FAIL("-from: '%s' is not a time in seconds", argv[i]);
            }
        } else if (strcmp(a, "-to") == 0) {
            NEED(1);
            if (!wbvRenderParseSeconds(argv[++i], &out->toMs)) {
                ARG_FAIL("-to: '%s' is not a time in seconds", argv[i]);
            }
            out->haveTo = true;
        } else if (strcmp(a, "-fps") == 0) {
            NEED(1);
            if (!parseInt(argv[++i], 1, WBV_RENDER_MAX_FPS, &out->fps)) {
                ARG_FAIL("-fps: '%s' is not a number from 1 to %d", argv[i],
                         WBV_RENDER_MAX_FPS);
            }
        } else if (strcmp(a, "-view") == 0) {
            NEED(4);
            if (!parseInt(argv[i + 1], 0, 255, &out->viewX) ||
                !parseInt(argv[i + 2], 0, 255, &out->viewY) ||
                !parseInt(argv[i + 3], 1, WBV_RENDER_MAX_SQUARES, &out->viewW) ||
                !parseInt(argv[i + 4], 1, WBV_RENDER_MAX_SQUARES, &out->viewH)) {
                ARG_FAIL("-view needs x y w h in map squares (x, y 0-%d; "
                         "w, h 1-%d)", WBV_RENDER_MAX_SQUARES - 1,
                         WBV_RENDER_MAX_SQUARES);
            }
            /* The view cannot go past the map edge. The screen code would
             * move it back and print nothing, so refuse it here. */
            if (out->viewX + out->viewW > WBV_RENDER_MAX_SQUARES ||
                out->viewY + out->viewH > WBV_RENDER_MAX_SQUARES) {
                ARG_FAIL("-view %d %d %d %d goes past the map edge: x + w and "
                         "y + h must be %d or less", out->viewX, out->viewY,
                         out->viewW, out->viewH, WBV_RENDER_MAX_SQUARES);
            }
            out->haveView = true;
            i += 4;
        } else if (strcmp(a, "-size") == 0) {
            NEED(2);
            if (!parseInt(argv[i + 1], 1, WBV_RENDER_MAX_SQUARES, &sizeW) ||
                !parseInt(argv[i + 2], 1, WBV_RENDER_MAX_SQUARES, &sizeH)) {
                ARG_FAIL("-size needs w h in map squares (1-%d)",
                         WBV_RENDER_MAX_SQUARES);
            }
            haveSize = true;
            i += 2;
        } else if (strcmp(a, "-follow") == 0) {
            NEED(1);
            out->follow = argv[++i];
            if (out->follow[0] == '\0') {
                ARG_FAIL("-follow needs a player name or number");
            }
        } else if (strcmp(a, "-self") == 0) {
            NEED(1);
            out->self = argv[++i];
        } else if (strcmp(a, "-scale") == 0) {
            NEED(1);
            if (!parseInt(argv[++i], 1, WBV_RENDER_MAX_SCALE, &out->scale)) {
                ARG_FAIL("-scale: '%s' is not a number from 1 to %d", argv[i],
                         WBV_RENDER_MAX_SCALE);
            }
        } else if (strcmp(a, "-quiet") == 0) {
            out->quiet = true;
        } else if (a[0] == '-' && a[1] != '\0') {
            ARG_FAIL("unknown option %s", a);
        } else if (out->inPath == NULL) {
            out->inPath = a;
        } else {
            ARG_FAIL("more than one input file ('%s' and '%s')", out->inPath, a);
        }
    }
#undef NEED
#undef ARG_FAIL

    if (out->inPath == NULL) {
        if (err != NULL && errLen > 0) {
            snprintf(err, errLen, "no input .wbv given");
        }
        return false;
    }
    if (!out->rawOut && out->outDir == NULL) {
        if (err != NULL && errLen > 0) {
            snprintf(err, errLen, "-out <dir> or -out - is required");
        }
        return false;
    }
    if (out->haveView && out->follow != NULL) {
        if (err != NULL && errLen > 0) {
            snprintf(err, errLen, "-view and -follow cannot be used together");
        }
        return false;
    }
    if (out->haveView && haveSize) {
        if (err != NULL && errLen > 0) {
            snprintf(err, errLen, "-size cannot be used with -view, which has its own size");
        }
        return false;
    }
    if (out->haveTo && out->toMs <= out->fromMs) {
        if (err != NULL && errLen > 0) {
            snprintf(err, errLen, "-to must be later than -from");
        }
        return false;
    }
    if (!out->haveView) {
        out->viewW = sizeW;
        out->viewH = sizeH;
    }
    return true;
}
