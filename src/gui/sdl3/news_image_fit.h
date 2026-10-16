/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          news_image_fit
 * Filename:      news_image_fit.h
 * Purpose:
 *   The size the news window draws an image at: its own
 *   size, shrunk to fit the space it has and keeping its
 *   shape, never enlarged. Header-only arithmetic so the
 *   unit tests can check it with no renderer.
 *********************************************************/

#ifndef NEWS_IMAGE_FIT_H
#define NEWS_IMAGE_FIT_H

#include <math.h>

/* Fits an image of imgW x imgH into maxW x maxH. Writes the size to
 * draw it at to *outW and *outH: the image's own size when it fits,
 * otherwise scaled down by one factor for both sides so it fits both
 * limits, rounded down and never below 1 x 1. Writes 0 x 0 when the
 * image has no size. A limit of 0 or less is treated as 1. */
static inline void newsImageFit(float imgW, float imgH,
                                float maxW, float maxH,
                                float *outW, float *outH) {
    if (!(imgW > 0.0f) || !(imgH > 0.0f)) {
        *outW = 0.0f;
        *outH = 0.0f;
        return;
    }
    if (!(maxW > 1.0f)) maxW = 1.0f;
    if (!(maxH > 1.0f)) maxH = 1.0f;

    float scale = 1.0f;
    if (imgW > maxW) scale = maxW / imgW;
    if (imgH * scale > maxH) scale = maxH / imgH;
    if (scale >= 1.0f) {
        *outW = imgW;
        *outH = imgH;
        return;
    }
    float w = floorf(imgW * scale);
    float h = floorf(imgH * scale);
    *outW = w < 1.0f ? 1.0f : w;
    *outH = h < 1.0f ? 1.0f : h;
}

#endif /* NEWS_IMAGE_FIT_H */
