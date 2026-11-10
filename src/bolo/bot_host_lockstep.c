/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The server's wait for the moves of its own Bot Host child
 * (bot_host_lockstep.h). */

#include "bot_host_lockstep.h"

#include <string.h>

void botHostLockstepReset(BotHostLockstep *ls) {
    if (ls != NULL) {
        memset(ls, 0, sizeof(*ls));
    }
}

void botHostLockstepArm(BotHostLockstep *ls, const bool *onChild,
                        const uint32_t *newCount, int n) {
    int i;
    if (ls == NULL) {
        return;
    }
    memset(ls->want, 0, sizeof(ls->want));
    ls->wanted = 0;
    if (onChild == NULL || newCount == NULL) {
        botHostLockstepReset(ls);
        return;
    }
    if (n > BOT_HOST_LOCKSTEP_SEATS) {
        n = BOT_HOST_LOCKSTEP_SEATS;
    }
    if (n < 0) {
        n = 0;
    }
    for (i = 0; i < BOT_HOST_LOCKSTEP_SEATS; i++) {
        if (i >= n || !onChild[i]) {
            /* Not the child's seat now: forget it. When it comes
             * back, it waits for its first new input. */
            ls->seen[i] = false;
            continue;
        }
        if (!ls->seen[i]) {
            ls->seen[i] = true;
            ls->last[i] = newCount[i];
            ls->idle[i] = BOT_HOST_LOCKSTEP_IDLE_ARMS;
        } else if (newCount[i] != ls->last[i]) {
            ls->last[i] = newCount[i];
            ls->idle[i] = 0;
        } else if (ls->idle[i] < BOT_HOST_LOCKSTEP_IDLE_ARMS) {
            ls->idle[i]++;
        }
        if (ls->idle[i] < BOT_HOST_LOCKSTEP_IDLE_ARMS) {
            ls->want[i] = true;
            ls->base[i] = newCount[i];
            ls->wanted++;
        }
    }
}

int botHostLockstepMissing(const BotHostLockstep *ls,
                           const uint32_t *newCount, int n) {
    int i, missing = 0;
    if (ls == NULL || newCount == NULL) {
        return 0;
    }
    if (n > BOT_HOST_LOCKSTEP_SEATS) {
        n = BOT_HOST_LOCKSTEP_SEATS;
    }
    for (i = 0; i < n; i++) {
        if (ls->want[i] && newCount[i] == ls->base[i]) {
            missing++;
        }
    }
    /* A seat past n (it left the table) is no longer waited for. */
    return missing;
}

BotHostLockstepStep botHostLockstepStep(int missing, int wanted,
                                        double waitedMs, int timeoutMs) {
    if (timeoutMs <= 0 || wanted <= 0) {
        return BOT_HOST_LOCKSTEP_NONE;
    }
    if (missing <= 0) {
        return BOT_HOST_LOCKSTEP_MET;
    }
    if (waitedMs >= (double)timeoutMs) {
        return BOT_HOST_LOCKSTEP_LATE;
    }
    return BOT_HOST_LOCKSTEP_WAIT;
}

bool botHostPendingExpired(uint32_t nowTick, uint32_t sinceTick,
                           uint64_t nowMs, uint64_t sinceMs) {
    return (uint32_t)(nowTick - sinceTick) > BOT_HOST_PENDING_TICKS &&
           nowMs - sinceMs > BOT_HOST_PENDING_MS;
}
