/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *Name:          Client Timing Estimator
 *Filename:      client_timing.h
 *Author:        John Morrison
 *Purpose:
 *  A single client-side estimator of server-clock offset, snapshot
 *  inter-arrival jitter, and round-trip time, fed from the data the UDP
 *  transport already receives.  Each estimate is min-over-window
 *  smoothed: a minimum filter rejects upward jitter (a delayed snapshot
 *  or a slow ping) and tracks the true floor.
 *
 *  This is a pure measurement module.  It has no transport, socket, or
 *  SDL dependency — it takes plain counters at three ingress points
 *  (join accept, snapshot header, pong) and exposes read accessors.  It
 *  is unit-tested standalone with synthetic sequences.  The jitter estimate
 *  is consumed by the render-clock interpolation (clientSimRenderPrepare ->
 *  interpRenderControl) to set the adaptive display delay; the clock offset,
 *  RTT, and pipeline depth are read-only (Net Info readout).
 *
 *  Both clocks share a unit: localTick and serverTick each advance at
 *  100Hz in 10ms units, so the clock offset is localArrivalTick -
 *  serverTick with no conversion.  Consecutive snapshots (~20ms apart)
 *  advance each counter by ~2.
 *********************************************************/

#ifndef CLIENT_TIMING_H
#define CLIENT_TIMING_H

#include <stdbool.h>
#include <stdint.h>

/* Backing array size for every min/max window.  The snapshot-derived
 * signals use the longer logical window; RTT uses a shorter one (set per
 * window at reset), both bounded by this capacity. */
#define CLIENT_TIMING_WINDOW_MAX 64

/* A small ring of recent samples with on-read min/max scan.  Identical in
 * spirit to PingMinWindow (transport_udp_internal.h) but kept here, widened
 * to int32_t and given a max scan, so this module stays free of the
 * transport header.  capacity (<= CLIENT_TIMING_WINDOW_MAX) is the logical
 * window length. */
typedef struct {
    int32_t samples[CLIENT_TIMING_WINDOW_MAX];
    uint8_t capacity; /* logical window length */
    uint8_t count;    /* valid samples, saturates at capacity */
    uint8_t head;     /* next slot to overwrite */
} ClientTimingWindow;

typedef struct {
    /* Server-clock offset, in 10ms tick units:
     *   offset = localArrivalTick - serverTick
     * (both counters share the 10ms/100Hz unit, so no conversion).  A
     * delayed snapshot inflates localArrivalTick, so the min over the
     * window selects the least-delayed sample — the floor offset.  The
     * absolute value carries the constant difference between the two free-
     * running counters; its stability is what a clean link should show. */
    ClientTimingWindow offsetWin;
    int32_t offsetTicks;  /* cached floor (min), or the join seed pre-snapshot */

    /* Pipeline depth = lastSentInputTick - lastProcessedInput, in
     * InputPacket.tick (10ms) units: the count of inputs the client has sent
     * but the server has not yet processed — the real in-flight depth.  Both
     * terms share the input-tick clock, so this is epoch-free (unlike
     * serverTick - lastProcessedInput, which mixes the server's tick clock with
     * the input-tick clock).  Min-over-window tracks the floor depth. */
    ClientTimingWindow depthWin;
    int32_t depthTicks;   /* cached floor (min) */

    /* Snapshot inter-arrival.  gapWin holds the per-snapshot gap in local
     * arrival ticks (10ms each; the nominal gap is ~2 between snapshots
     * ~20ms apart).  The floor is min(gapWin) — the nominal spacing; jitter
     * is max(gapWin) - min(gapWin) — the spread of arrival times above that
     * floor. */
    bool     haveArrival;
    uint32_t prevArrivalTick;
    ClientTimingWindow gapWin;
    int32_t  gapFloorTicks; /* cached min(gapWin) */
    int32_t  jitterTicks;   /* cached max(gapWin) - min(gapWin) */

    /* RTT in milliseconds, min-over-window — the floor RTT, same raw sample
     * the ping smoothers consume. */
    ClientTimingWindow rttWin;
    int32_t rttMs;        /* cached floor (min) */
} ClientTiming;

/* Clear to a defined zero state and (re)establish each window's logical
 * length.  Safe to call before any sample is folded. */
void clientTimingReset(ClientTiming *t);

/* Optional fast seed for the clock offset at join time, from the
 * JOIN_ACCEPT serverTick (local arrival tick is ~0 at accept).  joinRttTicks
 * is the join round-trip in local (10ms) ticks if a clean measurement is
 * available, else 0 — the snapshot-derived offset is the durable signal and
 * supersedes the seed once the first snapshot folds in. */
void clientTimingSeedFromJoin(ClientTiming *t, uint32_t serverTickAtAccept,
                              uint32_t joinRttTicks);

/* Fold one snapshot header: refines the clock offset from (serverTick,
 * localArrivalTick), the pipeline depth from (lastSentInputTick,
 * lastProcessedInput) — both in InputPacket.tick space — and the inter-arrival
 * jitter from the gap since the previous snapshot's localArrivalTick. */
void clientTimingOnSnapshot(ClientTiming *t, uint32_t serverTick,
                            uint32_t lastProcessedInput,
                            uint32_t lastSentInputTick,
                            uint32_t localArrivalTick);

/* Fold one RTT sample (milliseconds) — the same raw pong sample the ping
 * smoothers receive. */
void clientTimingOnRtt(ClientTiming *t, uint16_t rttMs);

/* Read accessors for the readout and future consumers. */

/* Clock offset, signed, in 10ms tick units (see struct comment). */
int32_t clientTimingClockOffsetTicks(const ClientTiming *t);

/* Pipeline depth, in InputPacket.tick (10ms) units. */
int32_t clientTimingPipelineDepthTicks(const ClientTiming *t);

/* Snapshot inter-arrival jitter, in milliseconds (gap spread * 10ms/tick). */
uint32_t clientTimingJitterMs(const ClientTiming *t);

/* Round-trip time floor, in milliseconds. */
uint32_t clientTimingRttMs(const ClientTiming *t);

#endif /* CLIENT_TIMING_H */
