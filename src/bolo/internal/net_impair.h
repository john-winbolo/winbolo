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

/* src/bolo/net_impair.h */

#ifndef NET_IMPAIR_H
#define NET_IMPAIR_H

#include <stdbool.h>
#include <stdint.h>

#include "platform_net.h"   /* struct sockaddr_in */

/*
 * Runtime, seedable network-impairment layer.  Wraps datagram send/recv
 * on either endpoint with configurable delay, jitter, loss and burst
 * loss so a developer can reproduce a lossy/laggy path on localhost.
 *
 * The module is pure: no sockets, no clock reads, no SDL.  The current
 * time is always passed in as a uint64_t nowMs.  All randomness comes
 * from the process-global bolo_rand() stream (src/bolo/public/bolo_rand.h)
 * — the module never seeds it — so a run made under a fixed bolo_srand()
 * seed produces the same drop/delay/reorder sequence every time.
 *
 * Per-datagram delay (base + per-packet jitter) is stored on each queued
 * packet, and netImpairPop releases the smallest-deliverAt packet that is
 * due.  Jitter therefore reorders packets naturally.
 */

typedef struct {
    uint32_t baseDelayMs;
    uint32_t jitterMs;
    uint32_t lossPercent;   /* 0-100 */
    uint32_t burstLossLen;  /* consecutive drops per loss event; min 1 */
} NetImpairConfig;

#define NET_IMPAIR_QUEUE_SIZE 512
#define NET_IMPAIR_MAX_PACKET 2048

typedef struct { /* one queued datagram */
    uint8_t data[NET_IMPAIR_MAX_PACKET];
    int len;
    struct sockaddr_in addr;
    uint64_t deliverAt;
    bool inUse;
} NetImpairPacket;

typedef struct {
    bool enabled;
    NetImpairConfig cfg;
    NetImpairPacket queue[NET_IMPAIR_QUEUE_SIZE];
    int count;
    uint32_t burstRemaining;
    uint32_t overflowDrops;  /* packets dropped because the queue was full */
} NetImpair;

/* Zero the layer and leave it disabled. */
void netImpairInit(NetImpair *ni);

/* Parse "delay=75,jitter=30,loss=2,burst=2".  All keys optional; defaults
 * 0/0/0/1.  Unknown key or malformed value returns false (out unspecified).
 * loss clamps to 0-100; burst clamps to a minimum of 1. */
bool netImpairParseConfig(const char *spec, NetImpairConfig *out);

/* Enable with the given config (re-initialises queue state). */
void netImpairEnable(NetImpair *ni, const NetImpairConfig *cfg);

bool netImpairEnabled(const NetImpair *ni);

/* Offer a datagram to the layer.  Returns false (caller sends/processes
 * directly, nothing copied) when the layer is disabled or the datagram is
 * larger than NET_IMPAIR_MAX_PACKET.  Otherwise the packet is dropped or
 * queued and true is returned. */
bool netImpairOffer(NetImpair *ni, const uint8_t *data, int len,
                    const struct sockaddr_in *addr, uint64_t nowMs);

/* Release the smallest-deliverAt packet whose deliverAt <= nowMs, copying
 * it out and freeing the slot.  Returns the packet length, or -1 if none
 * is due. */
int  netImpairPop(NetImpair *ni, uint8_t *buf, int maxLen,
                  struct sockaddr_in *addr, uint64_t nowMs);

#endif /* NET_IMPAIR_H */
