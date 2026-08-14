/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Ping Display Conditioning
 *Filename:      ping_display.h
 *Author:        John Morrison
 *Purpose:
 *  Turns the raw per-player RTT that rides in every tank
 *  snapshot into a number that is readable on screen.
 *
 *  The wire value is a single unsmoothed RTT sample: the
 *  server re-stamps it into every snapshot (50Hz) but only
 *  re-measures it once per PING/PONG (~0.4s, PING_INTERVAL_TICKS).
 *  Rendered raw it swings by tens of ms between measurements and
 *  strobes the colour bands for anyone parked near a threshold,
 *  so the player-facing rows run it through here first:
 *
 *    1. EWMA folded once per *changed* sample. Folding at the
 *       50Hz repeat rate would converge onto each sample within a
 *       few ticks and smooth nothing; at the ~0.4s measurement
 *       cadence, alpha=1/4 settles in ~1.5-2s.
 *    2. A repaint deadband: the shown value only moves when the
 *       smoothed value has drifted PING_DISPLAY_STEP_MS away from
 *       it, then snaps to the nearest step. Small wobble around a
 *       rounding boundary can't flip the digits back and forth.
 *    3. Colour-band hysteresis: crossing a threshold needs
 *       PING_BAND_HYSTERESIS_MS of overshoot, so a player sitting
 *       at ~50ms doesn't strobe green/yellow.
 *
 *  Display only. Lag compensation (ServerSim::playerPing) and
 *  shell forward-projection (ClientSim::projectionPingMs) read the
 *  raw and min-over-window values respectively and must never be
 *  fed from here — smoothing them would change what the sim does,
 *  not just what the player reads.
 *********************************************************/

#ifndef PING_DISPLAY_H
#define PING_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

/* Colour bands for the player-row ping text. Ordered best-to-worst so
 * the hysteresis logic can compare two bands for direction of travel. */
typedef enum {
    PING_BAND_NONE = 0,  /* no measurement yet — rows render "---" */
    PING_BAND_GOOD,      /* below PING_BAND_GOOD_MAX_MS */
    PING_BAND_FAIR,      /* below PING_BAND_FAIR_MAX_MS */
    PING_BAND_POOR
} PingBand;

#define PING_BAND_GOOD_MAX_MS   50
#define PING_BAND_FAIR_MAX_MS   150

/* Overshoot required before a band flip sticks. */
#define PING_BAND_HYSTERESIS_MS 10

/* Quantisation step (and deadband width) for the displayed number.
 * Values below one step render exactly — a 2ms LAN ping should read
 * "2ms", not get rounded up to the step or down into the 0 ("---")
 * sentinel that the omit-zero wire encoding already uses. */
#define PING_DISPLAY_STEP_MS    5

/* Per-player display state. Zero-initialised state is valid (equivalent
 * to pingDisplayReset), so an enclosing struct cleared by memset needs no
 * explicit init. */
typedef struct {
    uint32_t ewmaQ8;      /* smoothed RTT, Q8 fixed point */
    uint16_t lastRawMs;   /* last wire sample, for change detection */
    uint16_t shownMs;     /* what the rows actually render */
    uint8_t  band;        /* PingBand, tracked with hysteresis */
    bool     init;        /* has the EWMA been seeded */
} PingDisplay;

void pingDisplayReset(PingDisplay *d);

/* Fold one wire sample in and return the value to display. Repeats of the
 * previous sample are ignored (see the EWMA note above); a zero sample
 * means "no measurement" and resets the state, so a slot reused by a new
 * player can't inherit the previous occupant's average. */
uint16_t pingDisplayPush(PingDisplay *d, uint16_t rawMs);

/* Current display value / band without folding a new sample. */
uint16_t pingDisplayValue(const PingDisplay *d);
PingBand pingDisplayBand(const PingDisplay *d);

/* Stateless threshold classification — no hysteresis. For call sites with
 * no per-player state to carry it, such as the lobby's slot mirror, whose
 * ~5s refresh is far too slow to strobe. */
PingBand pingBandClassify(uint16_t pingMs);

#endif /* PING_DISPLAY_H */
