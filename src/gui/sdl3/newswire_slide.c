/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          newswire_slide.c
 * Purpose:       The newswire's slide on and off the bottom
 *                edge — see newswire_slide.h.
 *********************************************************/

#include "newswire_slide.h"

float newswireSlideStep(NewswireSlide *slide, uint64_t now,
                        uint64_t lastMessageTick, bool autoHide) {
    if (slide == 0) return 1.0f;

    bool wanted = !autoHide ||
                  (lastMessageTick != 0 && now >= lastMessageTick &&
                   now - lastMessageTick < NEWSWIRE_HOLD_MS);

    float step;
    if (slide->tick == 0 || now < slide->tick) {
        step = 1.0f;
    } else {
        step = (float)(now - slide->tick) / (float)NEWSWIRE_SLIDE_MS;
    }
    slide->tick = now;

    slide->off += wanted ? -step : step;
    if (slide->off < 0.0f) slide->off = 0.0f;
    if (slide->off > 1.0f) slide->off = 1.0f;
    return slide->off;
}
