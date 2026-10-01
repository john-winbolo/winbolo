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
 *Name:          Ping Display Conditioning
 *Filename:      ping_display.c
 *Author:        John Morrison
 *Purpose:       Smoothing, deadband and colour-band hysteresis
 *               for the player-facing ping readout. See
 *               ping_display.h for why each stage exists.
 *********************************************************/

#include "ping_display.h"

#include <string.h>

/* Same shape as the transport's PingEwma (alpha 1/4 in Q8) — kept separate
 * because that one lives behind the transport internals and folds at the
 * PONG rate, while this folds off the snapshot stream on its own clock. */
static uint16_t pingDisplayFold(PingDisplay *d, uint16_t sample) {
    if (!d->init) {
        d->ewmaQ8 = (uint32_t)sample << 8;
        d->init = true;
    } else {
        int32_t delta = (int32_t)((uint32_t)sample << 8) - (int32_t)d->ewmaQ8;
        d->ewmaQ8 = (uint32_t)((int32_t)d->ewmaQ8 + delta / 4);
    }
    return (uint16_t)((d->ewmaQ8 + 128) >> 8);
}

/* Snap to the nearest PING_DISPLAY_STEP_MS, except below one step where the
 * exact value is more honest than rounding to 0 or to the step. */
static uint16_t pingDisplayQuantise(uint16_t ms) {
    if (ms < PING_DISPLAY_STEP_MS) {
        return ms;
    }
    return (uint16_t)(((ms + PING_DISPLAY_STEP_MS / 2) / PING_DISPLAY_STEP_MS) *
                      PING_DISPLAY_STEP_MS);
}

/* Band for `ms`, requiring PING_BAND_HYSTERESIS_MS of overshoot past the
 * threshold being crossed before leaving `prev`. Seeding (prev == NONE) and
 * a two-band jump both take the fresh classification directly. */
static PingBand pingDisplayBandStep(PingBand prev, uint16_t ms) {
    PingBand fresh = pingBandClassify(ms);

    if (prev == PING_BAND_NONE || fresh == prev) {
        return fresh;
    }

    if (fresh > prev) {
        /* Worsening: hold the better band until the sample clears the
         * threshold by the margin. */
        if (prev == PING_BAND_GOOD &&
            ms < PING_BAND_GOOD_MAX_MS + PING_BAND_HYSTERESIS_MS) {
            return prev;
        }
        if (prev == PING_BAND_FAIR &&
            ms < PING_BAND_FAIR_MAX_MS + PING_BAND_HYSTERESIS_MS) {
            return prev;
        }
    } else {
        /* Improving: hold the worse band until the sample drops below the
         * threshold by the margin. */
        if (prev == PING_BAND_POOR &&
            ms > PING_BAND_FAIR_MAX_MS - PING_BAND_HYSTERESIS_MS) {
            return prev;
        }
        if (prev == PING_BAND_FAIR &&
            ms > PING_BAND_GOOD_MAX_MS - PING_BAND_HYSTERESIS_MS) {
            return prev;
        }
    }

    return fresh;
}

void pingDisplayReset(PingDisplay *d) {
    if (d != NULL) {
        memset(d, 0, sizeof(*d));
    }
}

uint16_t pingDisplayPush(PingDisplay *d, uint16_t rawMs, uint32_t nowMs) {
    uint16_t smoothed;
    int32_t drift;

    if (d == NULL) {
        return 0;
    }

    /* No measurement: drop back to the "---" state rather than holding a
     * stale average that a rejoining slot would then inherit. */
    if (rawMs == 0) {
        pingDisplayReset(d);
        return 0;
    }

    /* Rate-limit the fold. The same measurement is re-stamped into every
     * snapshot, so folding each arrival would walk the average onto it
     * within a few ticks. Unsigned arithmetic wraps correctly. */
    if (d->init && (nowMs - d->lastFoldMs) < PING_DISPLAY_FOLD_INTERVAL_MS) {
        return d->shownMs;
    }
    d->lastFoldMs = nowMs;

    smoothed = pingDisplayFold(d, rawMs);

    /* Deadband against the *unquantised* smoothed value: comparing the
     * quantised candidate instead would let a smoothed value wobbling
     * across a rounding boundary flip the digits every sample. */
    drift = (int32_t)smoothed - (int32_t)d->shownMs;
    if (d->shownMs == 0 || drift >= PING_DISPLAY_STEP_MS ||
        drift <= -PING_DISPLAY_STEP_MS) {
        d->shownMs = pingDisplayQuantise(smoothed);
    }

    d->band = (uint8_t)pingDisplayBandStep((PingBand)d->band, d->shownMs);
    return d->shownMs;
}

uint16_t pingDisplayValue(const PingDisplay *d) {
    return (d != NULL) ? d->shownMs : 0;
}

PingBand pingDisplayBand(const PingDisplay *d) {
    return (d != NULL) ? (PingBand)d->band : PING_BAND_NONE;
}

PingBand pingBandClassify(uint16_t pingMs) {
    if (pingMs == 0) {
        return PING_BAND_NONE;
    }
    if (pingMs < PING_BAND_GOOD_MAX_MS) {
        return PING_BAND_GOOD;
    }
    if (pingMs < PING_BAND_FAIR_MAX_MS) {
        return PING_BAND_FAIR;
    }
    return PING_BAND_POOR;
}
