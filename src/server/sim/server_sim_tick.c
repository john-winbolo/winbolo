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
 *Name:          Server Simulation Tick
 *Filename:      server_sim_tick.c
 *Author:        John Morrison
 *Purpose:
 *  The authoritative tick — input translation and the
 *  per-player queue drain, lag compensation, the world
 *  update, and the per-frame entry points the server
 *  calls into.
 *********************************************************/

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "server_sim_shared.h"      /* serverSimSetActive and simMapChangeCallback, the base/pill collectors, publishServerMessage, serverSimWinningOwner */
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"   /* serverSimStartGame, serverSimReturnToLobby, serverSimEnterGameOver — the state-machine transitions */
#include "log.h"                    /* logIsRecording, logAddEvent, logWriteTick — the per-tick .wbv and spectator-ring tap */
#include "log_internal.h"           /* logHasSpectatorRing — runs the spectator-ring tap when no .wbv is recording */
#include "interpolation.h"          /* INTERP_BUFFER_MS — the lag-compensation and gunsight-delay budget */
#include "screenbullet.h"           /* the screenBullets list the per-tick shell and explosion sweep fills */
#include "playersrejoin.h"          /* playersRejoinUpdate — the per-tick rejoin timer */
#include "treegrow.h"               /* treeGrowUpdate — the world update's tree growth */
#include "../../common/mp_diag_log.h"   /* mpDiagLog — the per-player input-drain trace */

/*********************************************************
 *NAME:          translateInputToTankButton
 *PURPOSE:
 *  Converts an InputPacket button bitmask to the existing
 *  tankButton enum value.
 *
 *ARGUMENTS:
 *  buttons - The bitmask from InputPacket.buttons
 *********************************************************/
static tankButton translateInputToTankButton(uint8_t buttons) {
    bool accel = (buttons & INPUT_BTN_ACCEL) != 0;
    bool decel = (buttons & INPUT_BTN_DECEL) != 0;
    bool left  = (buttons & INPUT_BTN_LEFT)  != 0;
    bool right = (buttons & INPUT_BTN_RIGHT) != 0;

    /* If both accel and decel, they cancel out */
    if (accel && decel) {
        accel = FALSE;
        decel = FALSE;
    }
    /* If both left and right, they cancel out */
    if (left && right) {
        left = FALSE;
        right = FALSE;
    }

    if (left && accel)  return TLEFTACCEL;
    if (right && accel) return TRIGHTACCEL;
    if (left && decel)  return TLEFTDECEL;
    if (right && decel) return TRIGHTDECEL;
    if (left)           return TLEFT;
    if (right)          return TRIGHT;
    if (accel)          return TACCEL;
    if (decel)          return TDECEL;
    return TNONE;
}

#ifdef WB_NETDEBUG
/* Net-debug rig: true when a tankButton carries a left/right turn
 * component, including the turn+accel/decel combos. Used to count
 * sim-executed turn half-steps. Test-only — never built in production. */
static bool netdebugButtonTurns(tankButton tb) {
    switch (tb) {
        case TLEFT:
        case TRIGHT:
        case TLEFTACCEL:
        case TRIGHTACCEL:
        case TLEFTDECEL:
        case TRIGHTDECEL:
            return true;
        default:
            return false;
    }
}
#endif

static void serverSimLogTick(ServerSim *sim) {
    BYTE count;

    /* Run whenever a .wbv log is recording OR a spectator ring is registered:
       both consume the per-tick location/shell events and the logWriteTick tap.
       A normal (non-recording) server still has a ring, so gating on .wbv alone
       would leave the ring empty and a connecting spectator with no seed. */
    if (logIsRecording() == FALSE && logHasSpectatorRing() == FALSE) return;

    /* Tank + LGM positions */
    for (count = 0; count < MAX_TANKS; count++) {
        if (sim->playerConnected[count] && sim->sim.tanks[count] != NULL) {
            BYTE mx = tankGetMX(&sim->sim.tanks[count]);
            BYTE my = tankGetMY(&sim->sim.tanks[count]);
            BYTE px = tankGetPX(&sim->sim.tanks[count]);
            BYTE py = tankGetPY(&sim->sim.tanks[count]);
            BYTE dir = tankGetDir(&sim->sim.tanks[count]);
            BYTE onBoat = (BYTE)tankIsOnBoat(&sim->sim.tanks[count]);
            logAddEvent(log_PlayerLocation, count, mx, my,
                        utilPutNibble(px, py),
                        utilPutNibble(dir, onBoat), NULL);

            if (lgmIsOut(&sim->sim.lgmen[count])) {
                logAddEvent(log_LgmLocation,
                            utilPutNibble(count, lgmGetFrame(&sim->sim.lgmen[count])),
                            lgmGetMX(&sim->sim.lgmen[count]),
                            lgmGetMY(&sim->sim.lgmen[count]),
                            utilPutNibble(lgmGetPX(&sim->sim.lgmen[count]),
                                          lgmGetPY(&sim->sim.lgmen[count])),
                            0, NULL);
            }

            /* Tank stocks, so a shot fired or a refill shows at the tick it
               happens rather than at the next snapshot. logAddEvent drops the
               record when none of the four changed since the last one written
               for this tank, so a still tank costs nothing. */
            logAddEvent(log_TankSetStock, count,
                        tankGetShells(&sim->sim.tanks[count]),
                        tankGetMines(&sim->sim.tanks[count]),
                        tankGetArmour(&sim->sim.tanks[count]),
                        tankGetTrees(&sim->sim.tanks[count]), NULL);
        }
    }

    /* Shells / explosions / tank explosions */
    {
        screenBullets sb = screenBulletsCreate();
        int entries, i;
        BYTE mx, my, px, py, frame;

        shellsCalcScreenBullets(&sim->sim.shs, &sb, 0, MAP_ARRAY_LAST, 0, MAP_ARRAY_LAST);
        explosionsCalcScreenBullets(&sim->sim.expl, &sb, 0, MAP_ARRAY_LAST, 0, MAP_ARRAY_LAST);
        tkExplosionCalcScreenBullets(&sim->sim.tankExplosions, &sb, 0, MAP_ARRAY_LAST, 0, MAP_ARRAY_LAST);

        entries = screenBulletsGetNumEntries(&sb);
        for (i = 1; i <= entries; i++) {
            screenBulletsGetItem(&sb, i, &mx, &my, &px, &py, &frame);
            logAddEvent(log_Shell, mx, my, utilPutNibble(px, py), frame, 0, NULL);
        }
        screenBulletsDestroy(&sb);
    }

    /* Periodic snapshot */
    if ((sim->tick % FULL_SYNC_INTERVAL) == 0) {
        logWriteSnapshot(sim, TRUE);
    }

    /* First entry of the running round is where the round's log segment — and
     * so every clip time measured against it — begins. Latched here rather than
     * derived from startDelay: the hold at the top of simRunHalfStep advances
     * the sim without writing anything, and its counter is in half-steps while
     * the value it was given is in the legacy 20 ms units, so no arithmetic on
     * it gives the right answer. Whichever tick actually wrote first is the
     * answer by definition. */
    if (sim->roundLogStartTick == ROUND_LOG_START_UNSET &&
        sim->state == serverStateRunning) {
        sim->roundLogStartTick = sim->tick;
    }

    /* The tick this entry is written at, so a viewer can turn playback time
     * into the clock a scenario's timer counts in. Queued after the snapshot
     * above and before logWriteTick, so it lands in this tick's own entry:
     * logWriteSnapshot has already flushed what came before it. Written at
     * the round's first entry, and again at every entry a snapshot was
     * written for, which is where a viewer can start playing from. */
    if (sim->state == serverStateRunning &&
        (sim->roundLogStartTick == sim->tick ||
         logSnapshotWrittenThisTick())) {
        logAddEvent(log_ServerTick,
                    (BYTE)((sim->tick >> 24) & 0xFF),
                    (BYTE)((sim->tick >> 16) & 0xFF),
                    (BYTE)((sim->tick >> 8) & 0xFF),
                    (BYTE)(sim->tick & 0xFF), 0, NULL);
    }

    logWriteTick();
}

uint8_t serverSimComputeLagCompTicks(uint32_t simTick, uint32_t viewTick,
                                     uint16_t pingMs) {
    uint32_t ticks;
    if (viewTick != 0 && viewTick <= simTick) {
        /* Rewind the real view age. posHistory records once per game tick
         * (20ms), so two server ticks map to one history entry. viewTick is
         * client-supplied, but so is pingMs below, so the trust model is
         * unchanged; the clamp bounds any abuse. */
        ticks = (simTick - viewTick) / 2;
    } else {
        /* viewTick unknown (0) or ahead of the server (stale/garbage): keep
         * the ping-based estimate — snapshot trip out (ping/2) + interp buffer. */
        ticks = (((uint32_t)pingMs / 2) + INTERP_BUFFER_MS) / 20;
    }
    if (ticks > LAG_COMP_MAX_TICKS) ticks = LAG_COMP_MAX_TICKS;
    return (uint8_t)ticks;
}

/* Apply a single input to player `count`'s tank: gap-fill for any ticks
 * lost to packet loss, per-input parity selection (keys vs game arm),
 * lag compensation, fire/mine/build, and the lastProcessedInput advance.
 *
 * Called from two sites: the normal dequeue (a real input,
 * isSubstitute == FALSE) and the stall branch (a substitute synthesised
 * from the last held buttons, isSubstitute == TRUE). A substitute carries
 * no actions and must never invent a one-shot, so for it the pending-
 * harvest merge, the action marker, the fire path, the input-flag handling
 * (autoslow / gunsight) and the lag-comp computation are all skipped — it
 * leaves the last real input's flag state in place and a substituted game
 * tick ends with lagCompTicks == 0 because it cannot shoot. */
static void serverSimApplyOneInput(ServerSim *sim, BYTE count,
                                   const InputPacket *in, bool isSubstitute) {
    /* `local` is a macro (#define local static, brain.h), so name the
     * working copy `applied`. */
    InputPacket applied = *in;
    bool inputIsKeys = (applied.tick % 2) == 1;
    tankButton tb = translateInputToTankButton(applied.buttons);

    /* Fill in gap ticks lost to packet loss.  When a UDP packet
     * is dropped, the dequeued tick jumps ahead (e.g. 98 → 101).
     * The missing ticks must still run so the turn ramp (firstLeft/
     * firstRight) stays in sync with the client's prediction.
     *
     * The window is what keeps a networked client's recovery jump out of
     * this. A client whose counter fell behind numbers its next input
     * lastProcessedInput + the round trip + CLIENT_INPUT_JUMP_MARGIN_HALFSTEPS
     * (client_snapshot.c); the server stall-advances through about the round
     * trip while that packet is in flight, so what is left when it lands is
     * the margin, and the margin is held above this window on purpose. A
     * jump filled here would run the margin's worth of half-steps with the
     * pre-hitch held buttons, which the client never predicted, and cost a
     * position correction on every jump. The two constants are a pair: move
     * one and read the other.
     *
     * The fill itself stays for every producer, and is what the in-process
     * ones need: a local producer supplies one input per two half-steps, so
     * the number it sends after a dry run is genuinely a half-step or two
     * past the last one the server ran, and those half-steps have to
     * happen. */
    {
        uint32_t expected = sim->lastProcessedInput[count] + 1;
        uint32_t gap = applied.tick - expected;
        if (gap > 0 && gap < 8) {
            tankButton gapTb = translateInputToTankButton(sim->lastInputButtons[count]);
            uint32_t gt;
            for (gt = expected; gt < applied.tick; gt++) {
                bool gapIsKeys = (gt % 2) == 1;
                sim->statGapFillTicks[count]++;
#ifdef WB_NETDEBUG
                if (netdebugButtonTurns(gapTb)) {
                    sim->dbgExecTurnTicks[count]++;
                }
#endif
                if (gapIsKeys) {
                    BYTE bmx = tankGetMX(&sim->sim.tanks[count]);
                    BYTE bmy = tankGetMY(&sim->sim.tanks[count]);
                    tankTurn(&sim->sim, &sim->sim.tanks[count], bmx, bmy, gapTb);
                } else {
                    sim->sim.lagCompTicks = 0;
                    tankUpdate(&sim->sim, &sim->sim.tanks[count], gapTb, FALSE, FALSE);
                }
            }
        }
    }

    /* Save buttons for stall continuity */
    sim->lastInputButtons[count] = applied.buttons;

    /* Flag handling is real-input only. A substitute carries flags = 0; the
     * old stall branch never ran these calls, so applying flags=0 would
     * force-disable autoslow each substituted tick and diverge from the
     * client's prediction. Skipping the block leaves the tank-side flag
     * state from the last real input in place — the old behavior. */
    if (!isSubstitute) {
        /* Apply autoslowdown state from client flags */
        tankSetAutoSlowdown(&sim->sim.tanks[count],
                            (applied.flags & INPUT_FLAG_AUTOSLOW) != 0);

        /* Apply gunsight adjustment (bits 2-3 of flags) */
        uint8_t gsAdj = (applied.flags & INPUT_FLAG_GUNSIGHT_MASK) >> INPUT_FLAG_GUNSIGHT_SHIFT;
        if (gsAdj == 1) {
            tankGunsightIncrease(NULL, &sim->sim, &sim->sim.tanks[count]);
        } else if (gsAdj == 2) {
            tankGunsightDecrease(NULL, &sim->sim, &sim->sim.tanks[count]);
        }
    }

    sim->lastProcessedInput[count] = applied.tick;

#ifdef WB_NETDEBUG
    /* One increment per executed half-step whose button turns,
     * covering both the keys arm and the game arm below. */
    if (netdebugButtonTurns(tb)) {
        sim->dbgExecTurnTicks[count]++;
    }
#endif

    if (inputIsKeys) {
        /* Keys tick: turning only */
        BYTE bmx = tankGetMX(&sim->sim.tanks[count]);
        BYTE bmy = tankGetMY(&sim->sim.tanks[count]);
        tankTurn(&sim->sim, &sim->sim.tanks[count], bmx, bmy, tb);
    } else {
        /* Game tick: full update (turning + accel + movement) */

        /* Fold in one-shot actions harvested off stall-dropped stale
         * entries (see the dequeue skip loop). Done here, in the game arm,
         * because mine/build are only acted on for game ticks — folding on
         * a keys tick would consume and silently drop a harvested mine.
         * Per-field with collision deferral so two separately commanded
         * one-shots never merge into one: if this input already carries the
         * same one-shot, leave the pending copy for a later game tick. A
         * substitute carries no actions and never receives pending state. */
        if (!isSubstitute) {
            if ((sim->pendingHarvestActions[count] & INPUT_ACTION_LAY_MINE) &&
                !(applied.actions & INPUT_ACTION_LAY_MINE)) {
                applied.actions |= INPUT_ACTION_LAY_MINE;
                sim->pendingHarvestActions[count] &= ~INPUT_ACTION_LAY_MINE;
            }
            if (sim->pendingHarvestBuildAction[count] != 0 &&
                applied.buildAction == 0) {
                /* Only replay the harvested build if its frozen target
                 * would still be accepted against the current map — the same
                 * question lgmAddRequest asks, put to lgmCheckNewRequest
                 * without acting or messaging. A stale replay onto changed
                 * terrain would dispatch a bogus request and nag the player
                 * about an order they did not just issue. Either way the
                 * pending slot is consumed exactly once.
                 *
                 * Ask only when the man is idle, because only then does
                 * lgmAddRequest act on the order now. A busy man has it
                 * queued as his next order and checked when he gets back in
                 * the tank, with whatever the tank holds by then — testing
                 * it now against a tank whose wood is out with the man would
                 * throw away an order that was going to succeed. */
                if (!lgmIsIdle(&sim->sim.lgmen[count]) ||
                    lgmRequestIsValid(
                        &sim->sim, &sim->sim.lgmen[count],
                        &sim->sim.tanks[count],
                        sim->pendingHarvestBuildX[count],
                        sim->pendingHarvestBuildY[count],
                        (BYTE)(sim->pendingHarvestBuildAction[count] - 1))) {
                    applied.buildAction = sim->pendingHarvestBuildAction[count];
                    applied.buildX      = sim->pendingHarvestBuildX[count];
                    applied.buildY      = sim->pendingHarvestBuildY[count];
                }
                sim->pendingHarvestBuildAction[count] = 0;
                sim->pendingHarvestBuildX[count] = 0;
                sim->pendingHarvestBuildY[count] = 0;
            }
        }

        bool shoot = (applied.actions & INPUT_ACTION_FIRE) != 0;
        if (isSubstitute) {
            /* A substitute cannot shoot, so it must leave 0 comp behind
             * (matching the old stall branch) rather than rewinding. */
            sim->sim.lagCompTicks = 0;
        } else {
            uint8_t compTicks = serverSimComputeLagCompTicks(
                sim->tick, applied.viewTick, sim->playerPing[count]);
            sim->sim.lagCompTicks = compTicks;
            sim->statLastRewindTicks[count] = compTicks;
        }
        /* Stamp the originating input tick so a shell created inside this
         * tankUpdate carries it (shellsAddItem reads sim->fireInputTick).
         * Reset to 0 immediately after so pill shells / later world systems
         * in this tick don't inherit a stale player tick. */
        sim->sim.fireInputTick = applied.tick;
        tankUpdate(&sim->sim, &sim->sim.tanks[count], tb, shoot, FALSE);
        sim->sim.fireInputTick = 0;

        /* Handle mine laying */
        if (applied.actions & INPUT_ACTION_LAY_MINE) {
            tankLayMine(&sim->sim, &sim->sim.tanks[count]);
#ifdef WB_NETDEBUG
            sim->dbgMineLays[count]++;
#endif
        }

        /* Handle LGM build requests.  buildAction is 1-based in
         * InputPacket (0=none, 1=BsTrees, 2=BsRoad, ...) but
         * lgmAddRequest expects 0-based enum values. */
        if (applied.buildAction != 0) {
            lgmAddRequest(&sim->sim, &sim->sim.lgmen[count],
                          &sim->sim.tanks[count],
                          applied.buildX,
                          applied.buildY,
                          applied.buildAction - 1);
        }
    }

    /* Action marker: a real apply that executed a one-shot (fire, mine,
     * or build, all game-arm only) advances lastActionAppliedTick so a
     * redundant duplicate of this same tick is later recognised as
     * already-executed and not harvested. Substitutes never execute a
     * one-shot, so they never touch the marker. */
    if (!isSubstitute && !inputIsKeys) {
        bool executedAction = (applied.actions & INPUT_ACTION_FIRE) ||
                              (applied.actions & INPUT_ACTION_LAY_MINE) ||
                              (applied.buildAction != 0);
        if (executedAction) {
            sim->lastActionAppliedTick[count] = applied.tick;
        }
    }
}

/* One half-step of the sim: dequeues one input per player and runs
 * world systems on game ticks (sim->tick % 2 == 0).  Two consecutive
 * half-steps form one 20ms frame and match the client's 100Hz keys/
 * game alternation.  Event buffers (sim->events, sim->mapEvents) are
 * NOT cleared here — that happens once at the top of serverSimTick so
 * events from both half-steps accumulate naturally into one frame's
 * worth of state for downstream consumers (UDP drain, in-process
 * snapshot poll). */
/* Backlog of fresh (not-yet-processed) input for `count`, measured as
 * newestQueuedTick - lastProcessedInput: how many ticks behind the newest
 * queued input the server is. Immune to the redundancy duplicates that
 * inflate raw head-tail depth — a tick resent in N packets sits in the
 * queue N times but contributes once here. Returns 0 when nothing fresh
 * is queued. */
static uint32_t serverSimFreshBacklog(ServerSim *sim, BYTE count) {
    uint32_t lpi = sim->lastProcessedInput[count];
    uint32_t newest = lpi;
    uint8_t i = sim->inputQueueTail[count];
    while (i != sim->inputQueueHead[count]) {
        uint32_t t = sim->inputQueue[count][i & (SERVER_INPUT_QUEUE_SIZE - 1)].tick;
        if (t > newest) newest = t;
        i++;
    }
    return newest - lpi;
}

/* Largest tick among `count`'s queued entries, or 0 when the queue is empty.
 * Unlike serverSimFreshBacklog this does not clamp to lastProcessedInput, so
 * it still names a tick when everything queued is stale. */
static uint32_t serverSimNewestQueuedTick(const ServerSim *sim, BYTE count) {
    uint32_t newest = 0;
    uint8_t i = sim->inputQueueTail[count];
    while (i != sim->inputQueueHead[count]) {
        uint32_t t = sim->inputQueue[count][i & (SERVER_INPUT_QUEUE_SIZE - 1)].tick;
        if (t > newest) newest = t;
        i++;
    }
    return newest;
}

/* Pop entries for `count` until a fresh input (tick > lastProcessedInput)
 * or the queue empties. Stale entries are dropped with one-shot harvest.
 * Returns TRUE with *out filled on fresh; FALSE on empty. */
static bool serverSimDequeueFresh(ServerSim *sim, BYTE count, InputPacket *out) {
    while (sim->inputQueueHead[count] != sim->inputQueueTail[count]) {
        uint8_t tail = sim->inputQueueTail[count] & (SERVER_INPUT_QUEUE_SIZE - 1);
        *out = sim->inputQueue[count][tail];
        sim->inputQueueTail[count]++;
        /* Newest tick ever popped, whichever way this entry goes below. The
         * boundary rebase reads it to tell an entry it has never taken from
         * a redundancy duplicate of one it has already applied. */
        if (out->tick > sim->newestDequeuedTick[count]) {
            sim->newestDequeuedTick[count] = out->tick;
        }
        if (out->tick > sim->lastProcessedInput[count]) {
            return TRUE;
        }
        /* Stale entry (tick <= lastProcessedInput): its movement was
         * already covered, either by a real apply or by a stall
         * substitute, so dropping it here is correct. But a one-shot
         * action commanded on a stall-substituted tick was NEVER
         * executed (the substitute carries no actions), so it must be
         * harvested and carried onto the next real input — exactly
         * once. The discriminator is lastActionAppliedTick: an entry
         * with tick > lastActionAppliedTick was never executed
         * (harvest it); an entry with tick <= lastActionAppliedTick is
         * an ordinary redundant duplicate of an already-applied action
         * (ignore it). Fire is deliberately excluded — it is
         * level-triggered and reload-gated, so re-issuing it carries no
         * benefit and only adds state. Mine + build only. */
        if (out->tick > sim->lastActionAppliedTick[count]) {
            if (out->actions & INPUT_ACTION_LAY_MINE) {
                sim->pendingHarvestActions[count] |= INPUT_ACTION_LAY_MINE;
            }
            if (out->buildAction != 0) {
                /* A later harvested build overwrites an earlier pending
                 * one — newest commanded build intent wins. */
                sim->pendingHarvestBuildAction[count] = out->buildAction;
                sim->pendingHarvestBuildX[count] = out->buildX;
                sim->pendingHarvestBuildY[count] = out->buildY;
            }
            sim->lastActionAppliedTick[count] = out->tick;
        }
        sim->statDroppedStaleInputs[count]++;  /* stale/duplicate entry discarded */
        if (out->buttons != sim->lastInputButtons[count] &&
            out->tick + 8 > sim->lastProcessedInput[count]) {
            sim->statDroppedEdge[count]++;
        }
    }
    return FALSE;
}

static void simRunHalfStep(ServerSim *sim) {
    BYTE count;
    bool isKeysTick;
    BYTE numTanks;
    tank tanksArray[MAX_TANKS];
    lgm *lgmPtrs[MAX_TANKS];
    InputPacket currentInputs[MAX_TANKS];
    bool hasInput[MAX_TANKS];

    /* Arm this thread's active-sim slot BEFORE the state gate, not after
     * it.  The non-running branches below are not inert: they run
     * serverSimGameVoteTick and logWriteTick, and logWriteTick's pre-tick
     * hook is where the dedicated-log writer drains work queued from
     * another thread (handleLobbyEnter), which reaches back for the sim
     * through serverSimGetActive().  With the assignment after the switch,
     * a lobby tick ran that drain against whatever sim this thread last
     * touched — on the SDL timer thread, a sim from an earlier server in
     * the same process. */
    serverSimSetActive(sim);

    /* In-game vote driver — runs in every state so timeouts, heartbeats,
     * and the post-pass 3/2/1 countdown keep firing in SP, host, and
     * dedicated builds alike (independent of transport tick). */
    serverSimGameVoteTick(sim, (uint64_t)SDL_GetTicks());

    /* State machine gate — only run simulation in running state */
    switch (sim->state) {
    case serverStateLobby:
        /* No simulation but still advance tick for periodic lobby broadcasts. */
        sim->tick++;
        logWriteTick();
        return;
    case serverStateCountdown:
        sim->countdownTicks--;
        if (sim->countdownTicks <= 0) {
            serverSimStartGame(sim);
        } else {
            /* One held seat's runner, built and parked here so the round that
             * follows fields it without building anything. This is the window
             * for it: the countdown simulates nothing and owes a snapshot to
             * nobody, so a build here costs no client a slow frame the way the
             * same build during play would.
             *
             * What it does cost is a stall. serverGameTimer reads the clock
             * once a callback and then runs every tick the clock says is owed,
             * back to back, so a callback that spends 200ms on a brain is
             * followed by a burst of the ticks it held up, and each of those
             * can build another seat. Six seats is not 1.2s added to the
             * countdown; it is a stall of about 1.2s in which no client
             * receives anything, after which the countdown count catches up in
             * one burst and the number clients are shown jumps. Sixteen seats
             * is about 3.2s, well inside CLIENT_TIMEOUT_TICKS.
             *
             * One build a tick, so the countdown count still moves between
             * them, and never on the tick that starts the round. */
            serverSimWarmOneHeldSeat(sim);
        }
        logWriteTick();
        return;
    case serverStateGameOver:
        sim->countdownTicks--;
        if (sim->countdownTicks <= 0) {
            serverSimReturnToLobby(sim);
        }
        return;
    case serverStateRunning:
        break; /* Fall through to existing simulation code */
    }

    playersRejoinUpdate();

    /* Install map change callback to emit EVENT_MAP_CHANGE during tick */
    mapSetChangeCallback(simMapChangeCallback);

    if (sim->startDelay > 0) {
        sim->startDelay--;
        sim->tick++;
        mapSetChangeCallback(NULL);
        return;
    }

    /* Periodic state snapshot (-snapjson). Counted over exactly the same
     * running half-steps as ticksRun below — placed ahead of the limit
     * checks on purpose, so the last interval boundary still fires on the
     * tick that then trips the tick limit and returns. Runs before this
     * half-step touches anything, i.e. on fully settled state. */
    if (sim->snapshotInterval > 0 && sim->snapshotCb != NULL) {
        sim->snapshotTicks++;
        if ((sim->snapshotTicks % sim->snapshotInterval) == 0) {
            sim->snapshotCb(sim);
        }
    }

    if (sim->gameLength > 0) {
        sim->gameLength--;
        if (sim->gameLength == 0) {
            mapSetChangeCallback(NULL);
            serverSimConsoleMessage("Game time limit reached.");
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    if (sim->tickLimit > 0) {
        sim->ticksRun++;
        if (sim->ticksRun >= sim->tickLimit) {
            char ticksMsg[64];
            snprintf(ticksMsg, sizeof(ticksMsg),
                     "Reached tick limit (%d). Exiting.",
                     (int)sim->tickLimit);
            sim->tickLimit = 0;
            mapSetChangeCallback(NULL);
            serverSimConsoleMessage(ticksMsg);
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    if (sim->gameTickLimit > 0) {
        sim->gameTicksRun++;
        if (sim->gameTicksRun >= sim->gameTickLimit) {
            char gameTicksMsg[64];
            snprintf(gameTicksMsg, sizeof(gameTicksMsg),
                     "Game tick limit reached (%d). Ending game.",
                     (int)sim->gameTickLimit);
            sim->gameTickLimit = 0;
            sim->gameTicksRun = 0;
            mapSetChangeCallback(NULL);
            serverSimConsoleMessage(gameTicksMsg);
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    /* End the round once the last human leaves, so the server drops back
     * to the lobby instead of looping a bot-only game forever (which it
     * otherwise does — the empty/auto-close checks count bots via
     * serverSimGetNumPlayers). Gated on roundHadHuman so a game that
     * legitimately started with only bots (all bots ready) isn't ended
     * the instant it starts, which would loop start<->gameover. Only for
     * lobby-enabled servers; a no-lobby game-over means shutdown, which a
     * transient human dropout shouldn't trigger. Suppress the win message
     * — nobody won, everyone left. Routes through the normal GAME_OVER ->
     * countdown -> returnToLobby flow (WBN swap, log flush, republish). */
    if (sim->lobbyEnabled) {
        if (serverSimGetNumHumans(sim) > 0) {
            sim->roundHadHuman = true;
        } else if (sim->roundHadHuman) {
            mapSetChangeCallback(NULL);
            sim->returnToLobbyReason = RETURN_REASON_ABANDONED;
            serverSimConsoleMessage("No human players remaining. Returning to lobby.");
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    /* Server tick parity controls world systems only.
     * Per-player keys vs game is determined by the INPUT's tick parity,
     * so client/server parity is always aligned regardless of when
     * the client joined. */
    isKeysTick = (sim->tick % 2) == 1;

    /* Dequeue one input per player for this tick, skipping duplicates.
     * Redundant UDP packets can queue the same tick number multiple times
     * because serverHandleInput's dedup check uses lastProcessedInput which
     * isn't updated until dequeue time.  Drain any stale/duplicate entries
     * so each tick number is only processed once. */
    for (count = 0; count < MAX_TANKS; count++) {
        hasInput[count] = FALSE;
        if (!sim->playerConnected[count]) {
            continue;
        }

        /* Calculate queue depth (inputs available) */
        uint8_t queueDepth = sim->inputQueueHead[count] - sim->inputQueueTail[count];

        /* Jitter buffer gate: wait until we have enough inputs buffered.
         * Once the buffer has filled initially, keep processing even if
         * depth drops to 1 (drain rather than stall). Only re-enter
         * buffering mode if the queue empties completely. */
        if (!sim->inputBufferFilled[count]) {
            if (queueDepth < sim->jitterTarget[count]) {
                continue;  /* Still filling to adaptive target */
            }
            sim->inputBufferFilled[count] = 1;
        }

        /* Break a stall-advance lockout. A dry run past the threshold
         * substitutes a tick every half-step, which lifts lastProcessedInput
         * past tick numbers the client has not produced yet, so everything
         * that then arrives is stale and the dry run never ends. When the
         * queue holds nothing this dequeue would take, move lastProcessedInput
         * back under the newest queued tick so that one entry is taken as
         * fresh. Only that one: the dequeue still drops every entry below it
         * as stale, so a tick already dequeued is not taken a second time.
         * The rebased tick itself is one a substitute has already moved the
         * tank through, so the overshoot guarantee is given up for it. That
         * is the trade: one tick of movement re-run against a dry run that
         * otherwise never ends. The apply resets inputDryTicks, which closes
         * this branch until the next dry run.
         *
         * newestDequeuedTick keeps a redundancy duplicate out of it. A resent
         * copy of an already-applied tick looks exactly like a never-taken
         * one here, and rebasing onto it would run its movement twice and lay
         * a second mine if it carried one. The UDP transport drops such a
         * copy before the queue; the in-process one does not dedup at all.
         *
         * A slot with no tank is left alone: loop 2 skips it, so nothing
         * would apply the rebased input and the rewound lastProcessedInput
         * would stay rewound. */
        if (sim->lastProcessedInput[count] > 0 &&
            sim->sim.tanks[count] != NULL &&
            sim->inputDryTicks[count] > STALL_ADVANCE_DRY_TICKS) {
            /* The queue walk is behind the two cheap tests: a healthy slot
             * is never this dry, and this runs for every slot every
             * half-step. */
            uint32_t newestQueued = serverSimNewestQueuedTick(sim, count);
            if (newestQueued > 0 &&
                newestQueued <= sim->lastProcessedInput[count] &&
                newestQueued > sim->newestDequeuedTick[count]) {
                sim->lastProcessedInput[count] = newestQueued - 1;
            }
        }

        /* Dequeue one input, skipping duplicates/stale */
        hasInput[count] = serverSimDequeueFresh(sim, count, &currentInputs[count]);

        /* Adaptive jitter buffer — track stalls and adjust target depth */
        if (sim->inputBufferFilled[count]) {
            if (!hasInput[count]) {
                /* Queue ran dry — we're consuming faster than inputs arrive */
                sim->jitterStallCount[count]++;
                sim->jitterStableTicks[count] = 0;
                if (sim->jitterStallCount[count] >= JITTER_GROW_THRESHOLD &&
                    sim->jitterTarget[count] < JITTER_BUFFER_MAX) {
                    sim->jitterTarget[count]++;
                    sim->jitterStallCount[count] = 0;
                }
            } else {
                sim->jitterStallCount[count] = 0;
                sim->jitterStableTicks[count]++;
                if (sim->jitterStableTicks[count] >= JITTER_SHRINK_INTERVAL &&
                    sim->jitterTarget[count] > JITTER_BUFFER_MIN) {
                    sim->jitterTarget[count]--;
                    sim->jitterStableTicks[count] = 0;
                    /* A long calm stretch also forgets recent drains, so the
                     * buffer is free to settle back toward MIN. */
                    sim->jitterStarveCount[count] = 0;
                }
            }
        }

        /* If queue drained completely, re-enter buffering mode. The drain
         * itself is the reliable too-shallow signal: under jitter the queue
         * empties faster than the grow path above can react, because once we
         * re-enter filling mode a later dry sub-tick takes the `continue`
         * above and never reaches that grow logic. Count drains separately
         * and deepen the buffer off them so jitter actually grows the target
         * instead of pinning it at the default. */
        if (sim->inputQueueHead[count] == sim->inputQueueTail[count] && !hasInput[count]) {
            if (sim->inputBufferFilled[count]) {
                sim->jitterStarveCount[count]++;
                if (sim->jitterStarveCount[count] >= JITTER_STARVE_GROW_THRESHOLD &&
                    sim->jitterTarget[count] < JITTER_BUFFER_MAX) {
                    sim->jitterTarget[count]++;
                    sim->jitterStarveCount[count] = 0;
                }
            }
            sim->inputBufferFilled[count] = 0;
        }
    }

    /* Per-player tank processing: use each input's tick parity */
    for (count = 0; count < MAX_TANKS; count++) {
        if (!sim->playerConnected[count] || sim->sim.tanks[count] == NULL) {
            continue;
        }
        sim->currentTickPlayer = count;
        if (hasInput[count]) {
            sim->inputDryTicks[count] = 0;
            serverSimApplyOneInput(sim, count, &currentInputs[count], FALSE);

            /* Backlog catch-up: bleed a standing queue at +1 input per sub-tick
             * (hard cap 2 applies total) so a jitter-spike backlog drains in ~1s
             * instead of ratcheting input latency for the session. Gate on the
             * fresh backlog (newest queued tick minus lastProcessedInput), not
             * raw head-tail depth: input redundancy resends each tick in several
             * packets, so the same unprocessed tick sits in the queue multiple
             * times and inflates raw depth — fresh backlog counts it once.
             * jitterTarget + 1 so steady-state never triggers it. */
            if (serverSimFreshBacklog(sim, count) > (uint32_t)(sim->jitterTarget[count] + 1)) {
                InputPacket extra;
                if (serverSimDequeueFresh(sim, count, &extra)) {
                    serverSimApplyOneInput(sim, count, &extra, FALSE);
                    sim->statCatchupTicks[count]++;
                }
            }
        } else {
            /* No fresh input this tick. Count every consecutive dry
             * half-step (including those where loop 1 left hasInput FALSE
             * mid-rebuffer) so the stall-advance gate sees the true dry
             * run, not just the jitter-buffer's stall count. */
            sim->inputDryTicks[count]++;
            tankButton stallTb = translateInputToTankButton(sim->lastInputButtons[count]);

            /* "Established at least once" is read off lastProcessedInput,
             * not the live inputBufferFilled flag: the dequeue loop above
             * clears inputBufferFilled on the very drain that produces this
             * stall (it re-enters buffering mode whenever the queue empties),
             * so by the time we get here it is already 0 on every genuine
             * stall — which is why the old statStallTicks gate on it was
             * dead. lastProcessedInput > 0 is the persistent signal — a real
             * input can only have advanced it after inputBufferFilled was
             * set, so it means the stream filled at least once. It gates both
             * the real-stall counter and the stall-advance: dead/loading
             * players that never streamed have it at 0, so they are not
             * counted as stalls and keep the idle path below. */
            if (sim->lastProcessedInput[count] > 0) {
                sim->statStallTicks[count]++;
            }

            /* Stall-advance only on a genuine multi-tick dry spell. A dry
             * run at/below STALL_ADVANCE_DRY_TICKS is routine send-burst
             * cadence ripple (the client batches 2 inputs/packet but the
             * server consumes 1 per half-step, so the queue drains to empty
             * for a half-step or two between packets); advancing there would
             * consume the tick and drop the in-flight real input as stale,
             * making the client reconcile constantly. Below the threshold we
             * wait without advancing the tank so the late input still applies
             * exactly once at its true tick. Only an established stream past
             * the threshold is treated as genuine loss and stall-advances. */
            if (sim->lastProcessedInput[count] > 0) {
                /* A short gap leaves the ACK unchanged. Advancing the tank
                 * here would add movement that replay cannot account for,
                 * then the delayed real input would move it again. Leave
                 * the whole tank state, including its turn ramp, intact. */
                if (sim->inputDryTicks[count] <= STALL_ADVANCE_DRY_TICKS) {
                    continue;
                }
                /* Established stream, genuine loss: stall-advance. A
                 * substituted tick is a *processed* tick — synthesise an
                 * input from the last held buttons at the next tick number
                 * and run it through the canonical apply, which advances
                 * lastProcessedInput past it. The real (late) input for this
                 * tick then arrives stale and its movement is dropped rather
                 * than executing the held turn a second time (the overshoot
                 * fix). The synth carries no actions, so it can never
                 * fire/lay/build, and isSubstitute suppresses the
                 * pending-harvest merge so a harvested one-shot waits for a
                 * real input. Parity comes from subTick (the apply body keys
                 * on its own tick), and the apply body counts the
                 * WB_NETDEBUG turn tick — so this branch must not count it
                 * again. */
                InputPacket synth;
                memset(&synth, 0, sizeof(synth));
                synth.tick      = sim->lastProcessedInput[count] + 1;
                synth.playerNum = count;
                synth.buttons   = sim->lastInputButtons[count];
                serverSimApplyOneInput(sim, count, &synth, TRUE);
            } else {
                /* A player that has not established an input stream still
                 * needs the idle simulation for death/respawn and loading. */
#ifdef WB_NETDEBUG
                if (netdebugButtonTurns(stallTb)) {
                    sim->dbgExecTurnTicks[count]++;
                }
#endif
                sim->sim.lagCompTicks = 0;
                if (isKeysTick) {
                    BYTE bmx = tankGetMX(&sim->sim.tanks[count]);
                    BYTE bmy = tankGetMY(&sim->sim.tanks[count]);
                    tankTurn(&sim->sim, &sim->sim.tanks[count], bmx, bmy, stallTb);
                } else {
                    tankUpdate(&sim->sim, &sim->sim.tanks[count], stallTb, FALSE, FALSE);
                }
            }
        }
    }

    /* Reset lagCompTicks after per-player loop so pill-fired shells get 0 */
    sim->sim.lagCompTicks = 0;

    /* World systems: run on even server ticks (game ticks) */
    if (!isKeysTick) {
        /* Update LGMs every game tick — server-authoritative state must
         * not depend on per-player input arrival.  Without this, parachute
         * descent, walking back to tank, and build progress freeze whenever
         * a player isn't sending fresh inputs (notably while dead). */
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.lgmen[count] != NULL
                && sim->sim.tanks[count] != NULL) {
                lgmUpdate(&sim->sim, &sim->sim.lgmen[count], &sim->sim.tanks[count]);
            }
        }

        /* Record tank positions for lag compensation history */
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.tanks[count] != NULL) {
                posHistoryRecord(&sim->posHistory[count],
                                 (*sim->sim.tanks[count]).x,
                                 (*sim->sim.tanks[count]).y,
                                 !tankIsDestroyed(&sim->sim.tanks[count]));
            }
        }

        /* Record LGM positions for lag compensation history */
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.lgmen[count] != NULL) {
                lgm lgman = sim->sim.lgmen[count];
                bool lgmAlive = !lgman->isDead && !lgman->inTank;
                posHistoryRecord(&sim->lgmPosHistory[count],
                                 lgman->x, lgman->y, lgmAlive);
            }
        }

        /* Update pillboxes — pass all tanks so pills can target closest enemy */
        pillsUpdate(&sim->sim, sim->sim.tanks, sim->playerConnected, MAX_TANKS);

        /* Base stock restocking */
        basesUpdate(&sim->sim, NULL);

        /* Server-authoritative base refueling */
        {
            BYTE numBases = basesGetNumBases(&sim->sim.bs);
            bool baseOccupied[MAX_BASES];
            BYTE b;

            memset(baseOccupied, FALSE, sizeof(baseOccupied));

            for (count = 0; count < MAX_TANKS; count++) {
                WORLD twx, twy;
                BYTE tx, ty, baseNum;
                if (!sim->playerConnected[count] || sim->sim.tanks[count] == NULL) {
                    continue;
                }
                if (tankIsDestroyed(&sim->sim.tanks[count])) {
                    continue;
                }
                tankGetWorld(&sim->sim.tanks[count], &twx, &twy);
                tx = (BYTE)(twx >> TANK_SHIFT_MAPSIZE);
                ty = (BYTE)(twy >> TANK_SHIFT_MAPSIZE);
                baseNum = basesGetBaseNum(&sim->sim.bs, tx, ty);
                if (baseNum != BASE_NOT_FOUND) {
                    baseOccupied[baseNum - 1] = TRUE;
                    if ((*sim->sim.bs).item[baseNum - 1].justStopped == FALSE) {
                        basesRefueling(&sim->sim, &sim->sim.tanks[count], baseNum);
                    } else {
                        (*sim->sim.bs).item[baseNum - 1].justStopped = FALSE;
                        (*sim->sim.bs).item[baseNum - 1].refuelTime = basesHalfTickCalulator(&sim->sim, BASES_HALFTICK_TYPE_ARMOUR);
                    }
                }
            }

            for (b = 0; b < numBases; b++) {
                if (!baseOccupied[b]) {
                    (*sim->sim.bs).item[b].justStopped = TRUE;
                }
            }
        }

        /* Precompute per-player compensation ticks for pill shell rewind */
        {
            BYTE c;
            for (c = 0; c < MAX_TANKS; c++) {
                uint16_t pingMs = sim->playerPing[c];
                uint16_t delayMs = (pingMs / 2) + INTERP_BUFFER_MS;
                uint8_t ticks = (uint8_t)(delayMs / 20);
                if (ticks > LAG_COMP_MAX_TICKS) ticks = LAG_COMP_MAX_TICKS;
                sim->sim.perPlayerCompTicks[c] = ticks;
            }
        }

        /* Enforce high-ping limits */
        transportUdpServerEnforcePing(sim);

        /* Build arrays for multi-tank subsystem updates.
         *
         * Snapshotted here — as late as possible, immediately before the
         * world-update stage that consumes them — and NOT earlier in the
         * half-step. tanksArray holds tank pointers by value and lgmPtrs holds
         * addresses of sim->sim.lgmen[] slots, so anything that removes a
         * player between this loop and the last consumer below leaves the
         * arrays pointing at freed objects (dangling tanks) or NULLed slots
         * (lgmen), with numTanks still counting the departed slot. That is
         * exactly what a mid-half-step ping kick used to do. Keep any code
         * that can call serverSimRemovePlayer above this point. */
        numTanks = 0;
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.tanks[count] != NULL) {
                tanksArray[numTanks] = sim->sim.tanks[count];
                lgmPtrs[numTanks] = &sim->sim.lgmen[count];
                numTanks++;
            }
        }

        /* Update world systems */
        /* tanksArray, NOT &sim->sim.tanks[0]: lgmPtrs is compacted over the
         * connected players, so the tank array must be compacted the same way
         * or the two index spaces diverge as soon as the occupied slots are
         * non-contiguous (anyone leaving mid-game). tkExplosionUpdate pairs
         * lgms[i] with tanks[i] to tell a killed lgm which tank to walk back
         * to, so a mismatch sent the man to another player's tank — or, when
         * the raw slot was empty, to a NULL tank and thus back to where he
         * died. shellsUpdate and minesExpUpdate below already take the
         * compacted array. */
        tkExplosionUpdate(&sim->sim, lgmPtrs, numTanks, tanksArray, &sim->sim.ss);
        shellsUpdate(&sim->sim, tanksArray, numTanks, lgmPtrs, &sim->sim.ss);
        {
            shells q = sim->sim.shs;
            while (q != NULL) {
                q->packSent = TRUE;
                q = q->next;
            }
        }
        minesExpUpdate(&sim->sim, lgmPtrs, numTanks, tanksArray, &sim->sim.ss);
        explosionsUpdate(&sim->sim.expl);
        floodUpdate(&sim->sim);
        for (count = 0; count < numTanks; count++) {
            treeGrowUpdate(&sim->sim);
        }
    }

    /* Clear map change callback */
    mapSetChangeCallback(NULL);

    /* Diff pills and emit update events for any that changed */
    {
        PillSnapshot currentPills[MAX_SNAPSHOT_PILLS];
        int np = serverSimGetPills(sim, currentPills, MAX_SNAPSHOT_PILLS);
        int p;
        for (p = 0; p < np; p++) {
            /* A removed pillbox is not on the map, so nothing names its
               index: the slot and the count stay put and the periodic full
               sync is what says how long the list is. */
            if (pillsIsActive(&sim->sim.pb, (BYTE)(p + 1)) == FALSE) {
                continue;
            }
            /* An index at or above prevPillCount is one the previous tick did
             * not have at all: a scenario created a pill (game.add_pill).
             * prevPills[] is zeroed only at sim create, so in round 2 those
             * slots still hold LAST round's final records — and an attacker
             * landing on the same shoreline tile and being handed its pill by
             * the same Lua call produces an identical 4-byte record. memcmp
             * then reports "unchanged" and the creation is never sent, leaving
             * every client a pill short until the next full sync. Count a new
             * index as changed rather than trust a compare against data from
             * another round. */
            if (p >= (int)sim->prevPillCount ||
                memcmp(&currentPills[p], &sim->prevPills[p], sizeof(PillSnapshot)) != 0) {
                GameEvent ev;
                ev.type = EVENT_PILL_UPDATE;
                memset(ev.data, 0, sizeof(ev.data));
                ev.data[0] = (uint8_t)p;
                ev.data[1] = currentPills[p].x;
                ev.data[2] = currentPills[p].y;
                ev.data[3] = currentPills[p].owner;
                ev.data[4] = currentPills[p].pillFlags;
                ev.data[5] = currentPills[p].armour;
                serverSimAddEvent(sim, &ev);
            }
        }
        memcpy(sim->prevPills, currentPills, np * sizeof(PillSnapshot));
        sim->prevPillCount = (uint8_t)np;
    }

    /* Diff bases and emit update events for any that changed */
    {
        BaseSnapshot currentBases[MAX_SNAPSHOT_BASES];
        int nb = serverSimGetBases(sim, currentBases, MAX_SNAPSHOT_BASES);
        int b;
        for (b = 0; b < nb; b++) {
            /* Same rule as the pillboxes above: a removed base's index is
               not named by an update or a stock line. */
            if (basesIsActive(&sim->sim.bs, (BYTE)(b + 1)) == FALSE) {
                continue;
            }
            /* Owner change: reliable, broadcast — everyone sees base colour. */
            if (currentBases[b].owner != sim->prevBases[b].owner) {
                GameEvent ev;
                ev.type = EVENT_BASE_UPDATE;
                memset(ev.data, 0, sizeof(ev.data));
                ev.data[0] = (uint8_t)b;
                ev.data[1] = currentBases[b].owner;
                serverSimAddEvent(sim, &ev);
            }
            /* Stock change: best-effort, culled per recipient to their closest base. */
            if (currentBases[b].armour != sim->prevBases[b].armour ||
                currentBases[b].shells != sim->prevBases[b].shells ||
                currentBases[b].mines  != sim->prevBases[b].mines) {
                GameEvent ev;
                ev.type = EVENT_BASE_STOCK;
                memset(ev.data, 0, sizeof(ev.data));
                ev.data[0] = (uint8_t)b;
                ev.data[1] = currentBases[b].armour;
                ev.data[2] = currentBases[b].shells;
                ev.data[3] = currentBases[b].mines;
                serverSimAddEvent(sim, &ev);
            }
        }
        memcpy(sim->prevBases, currentBases, nb * sizeof(BaseSnapshot));
        sim->prevBaseCount = (uint8_t)nb;
    }

    /* All-bases win. Every base held by one alliance with none of them dead
     * (armour > base_capture_armour — the same test basesGetStatusNum uses to
     * draw the X) IS the win condition, so it ends the round on the spot.
     *
     * There is deliberately no grace period layered on top. The grace period
     * is already built into the condition: a base shelled to 0 stays dead,
     * and therefore keeps the sweep false, for the whole time it takes to
     * regenerate past base_capture_armour. That is the losing side's window to
     * retake it. A second countdown on top only bought the right to announce
     * a win and then retract it.
     *
     * A no-lobby round has nowhere to return to — quitOnWin ends it the same
     * way and the process shuts down.
     *
     * A return-to-lobby countdown already running (a passed vote, a
     * surrender) is left alone: those are irrevocable decisions and own the
     * reason the returning lobby is given. The sweep must not relabel a
     * surrender's win credit on its way out. */
    if (sim->lobbyEnabled) {
        if (sim->returnToLobbyTicks == 0 && serverSimCheckGameWin(sim, FALSE)) {
            char buf[256];
            char name[256];
            BYTE winner = serverSimWinningOwner(sim);
            playersGetPlayerName(&sim->sim.plyrs, winner, name, sizeof(name),
                                 TRUE);
            snprintf(buf, sizeof(buf),
                     "*** %s and their allies control every base. ***", name);
            publishServerMessage(sim, buf);
            serverSimConsoleMessage(buf);
            /* Set before entering game over: serverSimResolveGameOver
             * switches on it to name the winner in the returning lobby and
             * credit the WinBolo.net win events. */
            sim->returnToLobbyReason = RETURN_REASON_BASE_WIN;
            mapSetChangeCallback(NULL);
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    } else if (sim->quitOnWin && serverSimCheckGameWin(sim, TRUE)) {
        mapSetChangeCallback(NULL);
        serverSimConsoleMessage("Game won!");
        serverSimEnterGameOver(sim);
        sim->tick++;
        return;
    }

    /* Forced return-to-lobby countdown (e.g. from a vote pass). Game
     * keeps running normally — players can move, shoot, etc. — and
     * each game tick this decrements. At 0 we transition to
     * gameOver, which is what triggers the existing lifecycle path
     * (broadcastGameOver + countdownTicks hold + returnToLobby). */
    if (sim->returnToLobbyTicks > 0) {
        sim->returnToLobbyTicks--;
        if (sim->returnToLobbyTicks == 0) {
            /* returnToLobbyReason, set when the countdown was armed, tells
             * serverSimResolveGameOver what the returning lobby is told. */
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    /* Refresh the proximity clocks the decay view policies read off. Returns
     * immediately unless some category is set to viewPolicyDecay. */
    serverSimUpdateViewDecay(sim);

    /* Drop any reported view whose target has stopped earning one — the rect
     * disappears and the client leaves the view by itself. */
    serverSimValidateViewTargets(sim);

    /* The legacy server ticked every 20ms (SERVER_TICK_LENGTH) and wrote
     * one log entry per tick.  Our sim ticks every 10ms alternating
     * keys/game.  Only log on game ticks (every 20ms) to match the
     * legacy rate — the log viewer consumes one entry per 20ms. */
    if (!isKeysTick) {
        serverSimLogTick(sim);
    }

    /* Per-second input-pipeline summary: one [netstat] line per connected
     * player, then reset that player's window counters. 100 sub-ticks =
     * 1 second. q and jt are live gauges read now; rewind is a gauge too
     * (most recent value, not reset). The rest accumulated over the window. */
    if ((sim->tick % 100) == 0) {
        for (count = 0; count < MAX_TANKS; count++) {
            if (!sim->playerConnected[count]) {
                continue;
            }
            {
                uint8_t qd = (sim->inputQueueHead[count] - sim->inputQueueTail[count])
                             & (SERVER_INPUT_QUEUE_SIZE - 1);
                mpDiagLog("[netstat] p%d q=%u jt=%u stall=%u gap=%u stale=%u dropEdge=%u catchup=%u rewind=%u",
                          count, qd, sim->jitterTarget[count],
                          sim->statStallTicks[count], sim->statGapFillTicks[count],
                          sim->statDroppedStaleInputs[count], sim->statDroppedEdge[count],
                          sim->statCatchupTicks[count],
                          sim->statLastRewindTicks[count]);
            }
            sim->statStallTicks[count] = 0;
            sim->statGapFillTicks[count] = 0;
            sim->statDroppedStaleInputs[count] = 0;
            sim->statDroppedEdge[count] = 0;
            sim->statCatchupTicks[count] = 0;
        }
    }

    sim->tick++;
}

/* Advance the sim by one 20ms frame.  In the running state this runs
 * two half-steps (the keys/game alternation that matches the client's
 * 100Hz input rate), with event buffers cleared once at the top so
 * events from both half-steps land in the same frame's worth of state
 * for downstream consumers.  In non-running states (lobby / countdown
 * / gameover) only one half-step runs, preserving the legacy state-
 * machine cadence — countdown durations, lobby refresh intervals, and
 * gameover return-to-lobby timing all stay calibrated against the
 * one-half-step-per-frame rate they were tuned for. */
void serverSimTick(ServerSim *sim) {
    if (sim->state == serverStateRunning) {
        sim->eventCount = 0;
        sim->mapEventCount = 0;
        /* Buffer pings accepted during this frame's packet receive: the
         * command-dispatch path runs before this clear, so it records the ping
         * rather than buffering it (anything buffered there would be wiped
         * before the post-tick UDP event drain sends it). Done here, right
         * after the clear, so both the snapshot build and the drain see it —
         * once. */
        serverSimFlushPendingPings(sim);
        simRunHalfStep(sim);
        simRunHalfStep(sim);
        /* THREE SHOTS = GO THERE, second half. The third shell only ARMS the
         * order; it is sent here, a quiet second after that shell was fired,
         * because a player who never shoots again has no other event left to
         * hang it on. After the half-steps, so a shell that died in this
         * frame is already counted. */
        serverSimShotOrderTick(sim);
        /* Ahead of the shadow tick so terrain a scenario edits from here
         * lands in the same frame's map events instead of the next one's.
         * The half-steps drop the map-change callback on their way out, so
         * it goes back on for the length of the hook and the fill drain and
         * comes off again before the shadow tick: without it a scenario's
         * terrain writes would reach the server's map and nobody else's. */
        mapSetChangeCallback(simMapChangeCallback);
        if (sim->scenarioTick != NULL) {
            sim->scenarioTick(sim->scenarioTickCtx);
        }
        /* After the hook, so a fill it has just started and the tail of an
         * older one are paced out of the one tile budget rather than each
         * spending a full one in the same frame. */
        serverSimScenarioDrainFill(sim);
        /* One roster change a frame, from the same queue the hook has just
         * added to. Inside the map callback's bracket because a bot joining
         * takes a copy of the terrain on its way in. */
        serverSimScenarioDrainRoster(sim);
        mapSetChangeCallback(NULL);
        /* Both half-steps have finished filling mapEvents and no transport has
         * drained them yet, so this is where each client's copy of the terrain
         * takes the frame's changes. Here rather than in the UDP drain so
         * in-process clients and bots, which never reach that drain, track the
         * same way. The non-running states run no simulation and emit no map
         * events (the change callback is only installed for a running tick),
         * so there is nothing to apply on that branch. */
        serverSimShadowTick(sim);
    } else {
        /* No map event is recorded on this branch, but the fill drain below
           reads the count as a bound, so it starts each tick at zero here as
           it does on a running frame. */
        sim->mapEventCount = 0;
        simRunHalfStep(sim);
        /* The half-step above is where a countdown runs out, so the state can
         * read running from here on and the rest of this frame is the round's
         * first. Terrain the hook or the fill writes then belongs to the round
         * and is recorded the way a running frame records it. The states that
         * are still not running publish no map events — a lobby client is
         * handed the whole map on download and again at the start — so the
         * callback stays off for them. */
        if (sim->state == serverStateRunning) {
            mapSetChangeCallback(simMapChangeCallback);
        }
        if (sim->scenarioTick != NULL) {
            sim->scenarioTick(sim->scenarioTickCtx);
        }
        /* A fill is paced here only so one frame's work stays one frame's
         * work. */
        serverSimScenarioDrainFill(sim);
        mapSetChangeCallback(NULL);
        if (sim->state == serverStateRunning) {
            /* The round started in this frame, so each slot's copy of the
             * terrain takes the frame's changes here, as it does at the end of
             * a running frame. The caller sends the same events straight
             * after this returns, and the next frame's clear above is what
             * would otherwise take them: a square sent to a client with no
             * copy advanced for it reads back as a checksum that never
             * settles. */
            serverSimShadowTick(sim);
        }
    }
}

/* Should intake admit this input although it is stale?
 *
 * A slot whose stream has fallen behind is stall-advanced every half-step
 * (see the stall branch in simRunHalfStep), which keeps lifting
 * lastProcessedInput past tick numbers the client has not produced yet.
 * Everything the client then sends is renumbered off a lastProcessedInput
 * that is half a round trip old, arrives at or below the current one, and is
 * dropped as stale — and only a fresh apply resets inputDryTicks, so the run
 * never ends on its own. The input that has to reach the queue is the newest
 * one the slot has ever seen, arriving stale while that run is going on: the
 * transport drops anything not strictly newer than lastProcessedInput, so
 * without this it never gets that far.
 *
 * This answers admission only. The rebase itself — moving lastProcessedInput
 * back under the newest queued tick — happens at the tick boundary in
 * simRunHalfStep, on one entry per dry run. Strictly newer than
 * newestInputTick, so the redundancy resends of one tick are admitted once. */
bool serverSimInputWouldRebase(const ServerSim *sim, BYTE n, uint32_t tick) {
    if (sim == NULL || n >= MAX_TANKS || !sim->playerConnected[n]) {
        return FALSE;
    }
    return tick > sim->newestInputTick[n] &&
           tick <= sim->lastProcessedInput[n] &&
           sim->inputDryTicks[n] > STALL_ADVANCE_DRY_TICKS;
}

void serverSimApplyInput(ServerSim *sim, const InputPacket *input) {
    BYTE p = input->playerNum;
    uint8_t head, next;
    InputPacket sanitized;
    if (p >= MAX_TANKS || !sim->playerConnected[p]) {
        return;
    }

    /* Sanitize input fields before queuing */
    sanitized = *input;
    sanitized.buttons &= (INPUT_BTN_ACCEL | INPUT_BTN_DECEL | INPUT_BTN_LEFT | INPUT_BTN_RIGHT);
    sanitized.actions &= (INPUT_ACTION_FIRE | INPUT_ACTION_LAY_MINE);
    /* buildAction is 1-based (0=none, 1=BsTrees..5=BsMine); zero out if invalid */
    if (sanitized.buildAction > 5) {
        sanitized.buildAction = 0;
    }
    /* flags: bit 0 = autoslow, bits 2-3 = gunsight adj (0=none, 1=increase, 2=decrease) */
    {
        uint8_t gsAdj = (sanitized.flags & INPUT_FLAG_GUNSIGHT_MASK) >> INPUT_FLAG_GUNSIGHT_SHIFT;
        if (gsAdj > 2) {
            sanitized.flags &= ~INPUT_FLAG_GUNSIGHT_MASK;
        }
        /* Mask off any undefined bits (keep only autoslow + gunsight) */
        sanitized.flags &= (INPUT_FLAG_AUTOSLOW | INPUT_FLAG_GUNSIGHT_MASK);
    }

    /* Newest tick ever received for this slot, whatever becomes of the input
     * afterwards. Intake asks serverSimInputWouldRebase against it so the
     * redundancy resends of one tick cannot each be admitted past a stale
     * check and flood the queue; the rebase that call is named for happens at
     * the tick boundary, in the dequeue loop. Maintained ahead of the
     * queue-full check below so an input dropped for want of room still
     * counts as received. */
    if (sanitized.tick > sim->newestInputTick[p]) {
        sim->newestInputTick[p] = sanitized.tick;
    }

    head = sim->inputQueueHead[p];
    next = (head + 1) & (SERVER_INPUT_QUEUE_SIZE - 1);
    /* Drop input if queue is full (shouldn't happen in practice) */
    if (next == (sim->inputQueueTail[p] & (SERVER_INPUT_QUEUE_SIZE - 1))) {
        return;
    }
    sim->inputQueue[p][head & (SERVER_INPUT_QUEUE_SIZE - 1)] = sanitized;
    sim->inputQueueHead[p] = head + 1;
    sim->playerPing[p] = transportUdpServerGetClientPing(p);
}

#ifdef WB_NETDEBUG
void serverSimNetdebugResetCounters(ServerSim *sim) {
    if (sim == NULL) return;
    memset(sim->dbgExecTurnTicks, 0, sizeof(sim->dbgExecTurnTicks));
    memset(sim->dbgMineLays, 0, sizeof(sim->dbgMineLays));
}

uint32_t serverSimNetdebugGetExecTurnTicks(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return 0;
    return sim->dbgExecTurnTicks[playerNum];
}

uint32_t serverSimNetdebugGetMineLays(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return 0;
    return sim->dbgMineLays[playerNum];
}
#endif
