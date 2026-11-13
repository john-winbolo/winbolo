/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Bot Host lockstep
 *Filename:      bot_host_lockstep.h
 *Purpose:
 *  The dedicated server's wait for the moves of its own
 *  Bot Host child. After the server sends tick N, it arms
 *  the wait: it keeps the input count of each seat that
 *  the child runs. Before the next tick, it waits until
 *  each of these seats sent a new input, or until the
 *  timeout. No network and no clock: the server gives the
 *  numbers, so the unit tests can check each rule.
 *  A seat that sent no new input for BOT_HOST_LOCKSTEP_IDLE_ARMS
 *  arms in a row is not waited for (its tank is dead, or
 *  its brain still loads). Its next new input makes it a
 *  waited seat again at the next arm. A new seat is waited
 *  for only after its first new input.
 *********************************************************/

#ifndef BOT_HOST_LOCKSTEP_H
#define BOT_HOST_LOCKSTEP_H

#include <stdbool.h>
#include <stdint.h>

#define BOT_HOST_LOCKSTEP_SEATS 16
#define BOT_HOST_LOCKSTEP_IDLE_ARMS 2

typedef struct {
    bool want[BOT_HOST_LOCKSTEP_SEATS];
    uint32_t base[BOT_HOST_LOCKSTEP_SEATS];
    int wanted;
    /* The history from arm to arm. */
    bool seen[BOT_HOST_LOCKSTEP_SEATS];
    uint32_t last[BOT_HOST_LOCKSTEP_SEATS];
    uint8_t idle[BOT_HOST_LOCKSTEP_SEATS];
} BotHostLockstep;

/* Clear the wait and the history. */
void botHostLockstepReset(BotHostLockstep *ls);

typedef enum {
    BOT_HOST_LOCKSTEP_NONE = 0, /* no seat to wait for */
    BOT_HOST_LOCKSTEP_MET,      /* each seat sent a new input */
    BOT_HOST_LOCKSTEP_LATE,     /* the timeout came first */
    BOT_HOST_LOCKSTEP_WAIT      /* wait more */
} BotHostLockstepStep;

/* Arm the wait. onChild[i]: seat i is a seat of the child. newCount[i]:
 * the count of new inputs of seat i now. n is at most
 * BOT_HOST_LOCKSTEP_SEATS (more seats are not waited for). The history
 * of the last arms stays (see the idle rule at the top). */
void botHostLockstepArm(BotHostLockstep *ls, const bool *onChild,
                        const uint32_t *newCount, int n);

/* The number of armed seats with no new input since the arm. */
int botHostLockstepMissing(const BotHostLockstep *ls,
                           const uint32_t *newCount, int n);

/* The choice: timeoutMs <= 0 means no wait (NONE). */
BotHostLockstepStep botHostLockstepStep(int missing, int wanted,
                                        double waitedMs, int timeoutMs);

/* An assign to a Bot Host that the host has not connected (a pending
 * seat) times out after BOT_HOST_PENDING_TICKS ticks AND
 * BOT_HOST_PENDING_MS of wall time. With -asap the ticks run faster than
 * 20 ms, and a host that loads its brains one by one needs the wall time.
 * Both counts wrap safely (unsigned differences). */
#define BOT_HOST_PENDING_TICKS 500u
#define BOT_HOST_PENDING_MS    10000u
bool botHostPendingExpired(uint32_t nowTick, uint32_t sinceTick,
                           uint64_t nowMs, uint64_t sinceMs);

#endif
