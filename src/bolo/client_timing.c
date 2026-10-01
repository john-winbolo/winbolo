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
 *Name:          Client Timing Estimator
 *Filename:      client_timing.c
 *Author:        John Morrison
 *Purpose:
 *  Implementation of the min-over-window client clock/jitter/RTT
 *  estimator.  See client_timing.h for the model and unit conventions.
 *********************************************************/

#include "client_timing.h"

#include <string.h>

/* localTick and serverTick both advance at 100Hz in 10ms units, so the clock
 * offset needs no conversion and the jitter ms conversion uses 10ms/tick. */
#define CLIENT_TIMING_LOCAL_TICK_MS 10

/* Logical window lengths.  Snapshots arrive ~50/s, so the snapshot window is
 * roughly a one-second floor; pongs arrive ~5/s (~0.2s cadence), so 8 samples
 * is a ~1.6s RTT floor. */
#define CLIENT_TIMING_SNAPSHOT_WINDOW 48
#define CLIENT_TIMING_RTT_WINDOW 8

/* Clamp a 64-bit tick difference into int32 range.  serverTick /
 * lastProcessedInput come straight off the wire (unpackU32), so a hostile or
 * buggy server can send values near 0x80000000; doing the difference in
 * int32 would risk signed-overflow UB (and negating INT32_MIN is UB too).
 * The legitimate differences are tiny, so the clamp only ever bites
 * pathological input — it just keeps the stored sample well-defined. */
static int32_t ctClampI32(int64_t v) {
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static void ctWindowInit(ClientTimingWindow *w, uint8_t capacity) {
    memset(w, 0, sizeof(*w));
    if (capacity > CLIENT_TIMING_WINDOW_MAX) {
        capacity = CLIENT_TIMING_WINDOW_MAX;
    }
    w->capacity = capacity;
}

static void ctWindowPush(ClientTimingWindow *w, int32_t sample) {
    if (w->capacity == 0) {
        return;
    }
    w->samples[w->head] = sample;
    w->head = (uint8_t)((w->head + 1) % w->capacity);
    if (w->count < w->capacity) {
        w->count++;
    }
}

static int32_t ctWindowMin(const ClientTimingWindow *w) {
    int32_t m;
    uint8_t i;
    if (w->count == 0) {
        return 0;
    }
    m = w->samples[0];
    for (i = 1; i < w->count; i++) {
        if (w->samples[i] < m) {
            m = w->samples[i];
        }
    }
    return m;
}

static int32_t ctWindowMax(const ClientTimingWindow *w) {
    int32_t m;
    uint8_t i;
    if (w->count == 0) {
        return 0;
    }
    m = w->samples[0];
    for (i = 1; i < w->count; i++) {
        if (w->samples[i] > m) {
            m = w->samples[i];
        }
    }
    return m;
}

void clientTimingReset(ClientTiming *t) {
    memset(t, 0, sizeof(*t));
    ctWindowInit(&t->offsetWin, CLIENT_TIMING_SNAPSHOT_WINDOW);
    ctWindowInit(&t->depthWin, CLIENT_TIMING_SNAPSHOT_WINDOW);
    ctWindowInit(&t->gapWin, CLIENT_TIMING_SNAPSHOT_WINDOW);
    ctWindowInit(&t->rttWin, CLIENT_TIMING_RTT_WINDOW);
    t->offsetTicks = 0;
    t->depthTicks = 0;
    t->haveArrival = false;
    t->prevArrivalTick = 0;
    t->gapFloorTicks = 0;
    t->jitterTicks = 0;
    t->rttMs = 0;
}

void clientTimingSeedFromJoin(ClientTiming *t, uint32_t serverTickAtAccept,
                              uint32_t joinRttTicks) {
    /* Local arrival tick is ~0 at accept, so the offset in 10ms units is
     * -serverTickAtAccept.  Half of the join round-trip (joinRttTicks is in
     * 10ms local ticks, so half is joinRttTicks / 2) advances the estimate
     * forward to account for the snapshot being half a round-trip stale.
     * joinRttTicks==0 leaves the raw seed. */
    t->offsetTicks = ctClampI32(-(int64_t)serverTickAtAccept +
                                (int64_t)(joinRttTicks / 2));
}

void clientTimingOnSnapshot(ClientTiming *t, uint32_t serverTick,
                            uint32_t lastProcessedInput,
                            uint32_t lastSentInputTick,
                            uint32_t localArrivalTick) {
    int32_t offset;
    int32_t depth;

    /* Clock offset in 10ms units: both counters share the unit, so it is
     * localArrivalTick - serverTick directly.  Min-over-window selects the
     * least-delayed snapshot (the smallest local arrival relative to the
     * server). */
    offset = ctClampI32((int64_t)localArrivalTick - (int64_t)serverTick);
    ctWindowPush(&t->offsetWin, offset);
    t->offsetTicks = ctWindowMin(&t->offsetWin);

    /* Pipeline depth in InputPacket.tick units: the inputs the client has sent
     * but the server has not yet processed.  Both terms share the input-tick
     * clock, so this is epoch-free.  Negative is not expected (lastProcessedInput
     * trails lastSentInputTick) but is left signed so a transient reorder doesn't
     * wrap. */
    depth = ctClampI32((int64_t)lastSentInputTick - (int64_t)lastProcessedInput);
    ctWindowPush(&t->depthWin, depth);
    t->depthTicks = ctWindowMin(&t->depthWin);

    /* Inter-arrival gap in local ticks.  The first snapshot only establishes
     * the baseline; from the second on, fold the gap. */
    if (t->haveArrival && localArrivalTick >= t->prevArrivalTick) {
        int32_t gap = ctClampI32((int64_t)localArrivalTick -
                                 (int64_t)t->prevArrivalTick);
        ctWindowPush(&t->gapWin, gap);
        t->gapFloorTicks = ctWindowMin(&t->gapWin);
        t->jitterTicks = ctWindowMax(&t->gapWin) - t->gapFloorTicks;
    }
    t->prevArrivalTick = localArrivalTick;
    t->haveArrival = true;
}

void clientTimingOnRtt(ClientTiming *t, uint16_t rttMs) {
    ctWindowPush(&t->rttWin, (int32_t)rttMs);
    t->rttMs = ctWindowMin(&t->rttWin);
}

int32_t clientTimingClockOffsetTicks(const ClientTiming *t) {
    return t->offsetTicks;
}

int32_t clientTimingPipelineDepthTicks(const ClientTiming *t) {
    return t->depthTicks;
}

uint32_t clientTimingJitterMs(const ClientTiming *t) {
    if (t->jitterTicks <= 0) {
        return 0;
    }
    return (uint32_t)t->jitterTicks * CLIENT_TIMING_LOCAL_TICK_MS;
}

uint32_t clientTimingRttMs(const ClientTiming *t) {
    if (t->rttMs <= 0) {
        return 0;
    }
    return (uint32_t)t->rttMs;
}
