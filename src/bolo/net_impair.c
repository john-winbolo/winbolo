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

/* src/bolo/net_impair.c — see net_impair.h for the contract. */

#include "net_impair.h"

#include <stdlib.h>
#include <string.h>

#include "bolo_rand.h"

void netImpairInit(NetImpair *ni) {
    memset(ni, 0, sizeof(*ni));
    ni->enabled = false;
}

bool netImpairParseConfig(const char *spec, NetImpairConfig *out) {
    NetImpairConfig cfg;
    char buf[256];
    size_t len;
    size_t i;

    if (spec == NULL || out == NULL) {
        return false;
    }

    /* Defaults for any key the spec omits. */
    cfg.baseDelayMs  = 0;
    cfg.jitterMs     = 0;
    cfg.lossPercent  = 0;
    cfg.burstLossLen = 1;
    cfg.dupPercent   = 0;

    len = strlen(spec);
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }
    memcpy(buf, spec, len + 1);

    i = 0;
    while (i < len) {
        size_t tokStart = i;
        size_t tokEnd;
        size_t eq;
        char *key;
        char *valStr;
        unsigned long val;
        char *endp;

        while (i < len && buf[i] != ',') {
            i++;
        }
        tokEnd = i;
        if (i < len) {
            i++;  /* step past the comma for the next token */
            if (i == len) {
                return false;  /* trailing comma — the final token is empty */
            }
        }
        if (tokEnd == tokStart) {
            return false;  /* empty token, e.g. trailing or doubled comma */
        }

        eq = tokStart;
        while (eq < tokEnd && buf[eq] != '=') {
            eq++;
        }
        if (eq == tokEnd || eq == tokStart) {
            return false;  /* no '=' or empty key */
        }
        buf[eq]     = '\0';
        buf[tokEnd] = '\0';  /* overwrites the consumed comma (or the NUL) */
        key    = &buf[tokStart];
        valStr = &buf[eq + 1];

        if (valStr[0] < '0' || valStr[0] > '9') {
            return false;  /* empty / signed / non-numeric value */
        }
        val = strtoul(valStr, &endp, 10);
        if (*endp != '\0') {
            return false;  /* trailing junk after the number */
        }

        if (strcmp(key, "delay") == 0) {
            cfg.baseDelayMs = (uint32_t)val;
        } else if (strcmp(key, "jitter") == 0) {
            cfg.jitterMs = (uint32_t)val;
        } else if (strcmp(key, "loss") == 0) {
            if (val > 100) {
                val = 100;
            }
            cfg.lossPercent = (uint32_t)val;
        } else if (strcmp(key, "burst") == 0) {
            if (val < 1) {
                val = 1;
            }
            cfg.burstLossLen = (uint32_t)val;
        } else if (strcmp(key, "dup") == 0) {
            if (val > 100) {
                val = 100;
            }
            cfg.dupPercent = (uint32_t)val;
        } else {
            return false;  /* unknown key */
        }
    }

    *out = cfg;
    return true;
}

void netImpairEnable(NetImpair *ni, const NetImpairConfig *cfg) {
    netImpairInit(ni);
    ni->cfg = *cfg;
    if (ni->cfg.burstLossLen < 1) {
        ni->cfg.burstLossLen = 1;
    }
    if (ni->cfg.lossPercent > 100) {
        ni->cfg.lossPercent = 100;
    }
    if (ni->cfg.dupPercent > 100) {
        ni->cfg.dupPercent = 100;
    }
    ni->enabled = true;
}

bool netImpairEnabled(const NetImpair *ni) {
    return ni->enabled;
}

bool netImpairOffer(NetImpair *ni, const uint8_t *data, int len,
                    const struct sockaddr_in *addr, uint64_t nowMs) {
    int i;
    NetImpairPacket *slot;
    uint32_t jitter;

    if (!ni->enabled) {
        return false;
    }

    /* Oversize datagrams bypass the queue — return false so the caller
     * sends directly.  We never truncate a packet to fit the slot. */
    if (len < 0 || len > NET_IMPAIR_MAX_PACKET) {
        return false;
    }

    /* Still draining the tail of an in-progress loss event. */
    if (ni->burstRemaining > 0) {
        ni->burstRemaining--;
        return true;
    }

    /* Roll for a fresh loss event. */
    if (bolo_rand_below(100) < ni->cfg.lossPercent) {
        ni->burstRemaining = ni->cfg.burstLossLen - 1;
        return true;
    }

    if (ni->count >= NET_IMPAIR_QUEUE_SIZE) {
        ni->overflowDrops++;
        return true;
    }

    slot = NULL;
    for (i = 0; i < NET_IMPAIR_QUEUE_SIZE; i++) {
        if (!ni->queue[i].inUse) {
            slot = &ni->queue[i];
            break;
        }
    }
    if (slot == NULL) {
        /* count < SIZE guarantees a free slot; defensive only. */
        ni->overflowDrops++;
        return true;
    }

    jitter = (ni->cfg.jitterMs > 0) ? bolo_rand_below(ni->cfg.jitterMs + 1) : 0;
    memcpy(slot->data, data, (size_t)len);
    slot->len       = len;
    slot->addr      = *addr;
    slot->deliverAt = nowMs + ni->cfg.baseDelayMs + jitter;
    slot->inUse     = true;
    ni->count++;

    /* Duplicate injection: on a hit, queue a second copy of the same
     * datagram with its own independently-rolled jitter, so the duplicate
     * can deliver before or after the original (reordering the two). */
    if (ni->cfg.dupPercent > 0 &&
        bolo_rand_below(100) < ni->cfg.dupPercent &&
        ni->count < NET_IMPAIR_QUEUE_SIZE) {
        NetImpairPacket *dup = NULL;
        for (i = 0; i < NET_IMPAIR_QUEUE_SIZE; i++) {
            if (!ni->queue[i].inUse) {
                dup = &ni->queue[i];
                break;
            }
        }
        if (dup != NULL) {
            uint32_t dupJitter = (ni->cfg.jitterMs > 0)
                                     ? bolo_rand_below(ni->cfg.jitterMs + 1)
                                     : 0;
            memcpy(dup->data, data, (size_t)len);
            dup->len       = len;
            dup->addr      = *addr;
            dup->deliverAt = nowMs + ni->cfg.baseDelayMs + dupJitter;
            dup->inUse     = true;
            ni->count++;
        }
    }
    return true;
}

int netImpairPop(NetImpair *ni, uint8_t *buf, int maxLen,
                 struct sockaddr_in *addr, uint64_t nowMs) {
    int i;
    int best = -1;
    uint64_t bestAt = 0;
    NetImpairPacket *slot;
    int copyLen;

    if (!ni->enabled || ni->count <= 0) {
        return -1;
    }

    for (i = 0; i < NET_IMPAIR_QUEUE_SIZE; i++) {
        if (!ni->queue[i].inUse) {
            continue;
        }
        if (ni->queue[i].deliverAt > nowMs) {
            continue;  /* not due yet */
        }
        if (best < 0 || ni->queue[i].deliverAt < bestAt) {
            best   = i;
            bestAt = ni->queue[i].deliverAt;
        }
    }
    if (best < 0) {
        return -1;
    }

    slot = &ni->queue[best];
    copyLen = slot->len;
    if (copyLen > maxLen) {
        copyLen = maxLen;
    }
    memcpy(buf, slot->data, (size_t)copyLen);
    if (addr != NULL) {
        *addr = slot->addr;
    }
    slot->inUse = false;
    ni->count--;
    return copyLen;
}
