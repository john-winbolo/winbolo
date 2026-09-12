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
 *Name:          Server Simulation Callbacks
 *Filename:      server_sim_callbacks.c
 *Author:        John Morrison
 *Purpose:
 *  The callbacks GameSim invokes on the server. All share
 *  the same shape — a void *ctx cast back to ServerSim * —
 *  and most turn the sim core's notification into a
 *  GameEvent for the snapshot stream or a ControlEvent for
 *  one client. serverSimInit wires them into
 *  sim->sim.callbacks.
 *********************************************************/

#include <string.h>
#include <stdlib.h>

#include "server_sim_shared.h"
#include "server_sim_internal.h"
#include "log.h"                  /* logAddEvent — the kill/death log entries */
#include "round_stats_derive.h"   /* roundStatsApplyRecord — the record callbacks' stats funnel */
#include "../../winbolonet/winbolonet_core.h"   /* winbolonetAddEvent — WBN kill tracking */

/* Forward declaration for server console message callback */
extern void serverMessageConsoleMessage(ServerSim *sim, char *msg);

/* Map an assistant body lang ID to the wire ID carried by
 * EVENT_ASSISTANT_MSG. Wire format unchanged — clients still receive
 * [targetPlayer, msgId] and resolve back to the matching LGM_* /
 * MESSAGE_* string locally. Returns 0 for any non-assistant body. */
static uint8_t assistantBodyIdToWireId(langid bodyId) {
    switch (bodyId) {
        case LGM_MAN_DEAD:               return ASSIST_MSG_MAN_DEAD;
        case LGM_NO_TREE:                return ASSIST_MSG_NO_TREE;
        case LGM_NO_BUILD:               return ASSIST_MSG_NO_BUILD;
        case LGM_NO_BUILD_UNDER_BOAT:    return ASSIST_MSG_NO_BUILD_BOAT;
        case LGM_INSUFFICIENT_TREES:     return ASSIST_MSG_INSUFFICIENT_TREES;
        case LGM_BUILDTANK:              return ASSIST_MSG_BUILDTANK;
        case LGM_PILL_NO_NEED_REPAIR:    return ASSIST_MSG_PILL_NO_REPAIR;
        case LGM_NO_PILLS:               return ASSIST_MSG_NO_PILLS;
        case LGM_INSUFFICIENT_MINES:     return ASSIST_MSG_INSUFFICIENT_MINES;
        case LGM_PILL_NO_BUILD_ON_MINE:  return ASSIST_MSG_PILL_ON_MINE;
        case MESSAGE_TANKSUNK:           return ASSIST_MSG_TANK_SUNK;
        default:                         return 0;
    }
}

/* Server-side messageAdd callback. Only assistant messages turn into
 * EVENT_ASSISTANT_MSG events; other message types (newswire, chat, AI)
 * are client-local — the server has no listener for them, so they are
 * silently dropped. */
void serverSimCbMessageAdd(void *ctx, messageType msgType,
                           langid topId, langid bodyId,
                           const MessageArgs *args) {
    ServerSim *sim = (ServerSim *)ctx;
    (void)topId;
    (void)args;

    if (msgType == assistantMessage) {
        uint8_t msgId = assistantBodyIdToWireId(bodyId);
        if (msgId != 0) {
            GameEvent ev;
            ev.type = EVENT_ASSISTANT_MSG;
            memset(ev.data, 0, sizeof(ev.data));
            ev.data[0] = sim->currentTickPlayer;
            ev.data[1] = msgId;
            serverSimAddEvent(sim, &ev);
        }
    }
}

/* Write the .wbv entry a sound maps to. Recording sits on the callback
 * because this is where the sound still carries its real map square: a human
 * client is sent a near/far tier and a bearing instead. Sounds with no log
 * type of their own add nothing. */
static void serverSimLogSound(sndEffects value, BYTE mx, BYTE my) {
    BYTE logMessageType = 0; /* Log item type */

    switch (value) {
        case shootSelf:
        case shootNear:
        case shootFar:
            logMessageType = log_SoundShoot;
            break;

        case shotTreeNear:
        case shotTreeFar:
            logMessageType = log_SoundFarm;
            break;

        case shotBuildingNear:
        case shotBuildingFar:
            /* No log entry: the format has no shot-building record. */
            break;

        case hitTankNear:
        case hitTankFar:
        case hitTankSelf:
            break;
        case bubbles:
        case tankSinkNear:
        case tankSinkFar:
            break;
        case bigExplosionNear:
            logMessageType = log_SoundExplosion;
            break;
        case bigExplosionFar:
            logMessageType = log_SoundExplosion;
            break;
        case farmingTreeNear:
        case farmingTreeFar:
            logMessageType = log_SoundFarm;
            break;
        case manBuildingNear:
        case manBuildingFar:
            logMessageType = log_SoundBuild;
            break;
        case manDyingNear:
        case manDyingFar:
            logMessageType = log_SoundManDie;
            break;

        case manLayingMineNear:
            logMessageType = log_SoundMineLay;
            break;

        case mineExplosionNear:
        case mineExplosionFar:
            logMessageType = log_SoundMineExplode;
            break;
    }

    if (logMessageType) {
        logAddEvent(logMessageType, mx, my, 0, 0, 0, NULL);
    }
}

void serverSimCbSoundDist(void *ctx, sndEffects value, BYTE mx, BYTE my) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    if (logIsRecording() == TRUE) {
        serverSimLogSound(value, mx, my);
    }
    ev.type = EVENT_SOUND;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)value;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = sim->currentTickPlayer;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbSoundDistShoot(void *ctx, BYTE mx, BYTE my, BYTE owner) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    if (logIsRecording() == TRUE) {
        serverSimLogSound(shootNear, mx, my);
    }
    ev.type = EVENT_SOUND_SHOOT;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)shootNear;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = owner;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbSoundDistTankHit(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_SOUND_TANK_HIT;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)hitTankNear;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = hitPlayer;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbMineVisible(void *ctx, BYTE mx, BYTE my, BYTE sourcePlayer) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_MINE_VISIBLE;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = mx;
    ev.data[1] = my;
    ev.data[2] = sourcePlayer;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbExplosion(void *ctx, BYTE mx, BYTE my, BYTE px, BYTE py) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_EXPLOSION;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = mx;
    ev.data[1] = my;
    ev.data[2] = px;
    ev.data[3] = py;
    serverSimAddEvent(sim, &ev);
}

/* A shell owned by `owner` ended (collision or expiry). Publish a
 * unicast CTRL_SHELL_DEATH so the firing client can match fireTick to
 * its predicted shell, cull the ghost, and draw the impact at
 * (impactWX, impactWY). udpClientDeliverControl filters to the owner. */
void serverSimCbShellDeath(void *ctx, uint32_t fireTick, BYTE owner,
                           WORLD impactWX, WORLD impactWY,
                           uint8_t outcome) {
    ServerSim *sim = (ServerSim *)ctx;
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SHELL_DEATH;
    evt.u.shellDeath.fireTick = fireTick;
    evt.u.shellDeath.impactWX = (uint16_t)impactWX;
    evt.u.shellDeath.impactWY = (uint16_t)impactWY;
    evt.u.shellDeath.owner    = owner;
    evt.u.shellDeath.outcome  = outcome;
    serverSimPublishControl(sim, &evt);
}

void serverSimCbTkExplosion(void *ctx, WORLD x, WORLD y,
                            TURNTYPE angle, BYTE length,
                            BYTE explodeType, BYTE creator) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_TK_EXPLOSION;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)((x >> 8) & 0xFF);
    ev.data[1] = (uint8_t)(x & 0xFF);
    ev.data[2] = (uint8_t)((y >> 8) & 0xFF);
    ev.data[3] = (uint8_t)(y & 0xFF);
    ev.data[4] = (uint8_t)angle;
    ev.data[5] = length;
    ev.data[6] = explodeType;
    ev.data[7] = creator;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbTankKill(void *ctx, BYTE killer, BYTE killed, BYTE deathCause, BYTE carriedPills) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_TANK_KILLED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = killer;
    ev.data[1] = killed;
    ev.data[2] = deathCause;
    ev.data[3] = carriedPills;
    /* Server-internal: the dying tank's carried trees, read by the stats
     * funnel. data[4] is past gameEventDataSize(), so it never goes on the wire. */
    ev.data[4] = (killed < MAX_TANKS && sim->sim.tanks[killed] != NULL)
                     ? tankGetTrees(&sim->sim.tanks[killed]) : 0;
    /* Server-internal: the dying tank's map cell (data[5]/data[6], also past
     * gameEventDataSize()), read by the funnel for the KILL record's mapX/mapY. */
    if (killed < MAX_TANKS && sim->sim.tanks[killed] != NULL) {
        WORLD kwx, kwy;
        tankGetWorld(&sim->sim.tanks[killed], &kwx, &kwy);
        ev.data[5] = (uint8_t)(kwx >> TANK_SHIFT_MAPSIZE);
        ev.data[6] = (uint8_t)(kwy >> TANK_SHIFT_MAPSIZE);
    }
    serverSimAddEvent(sim, &ev);
    winbolonetAddEvent(WINBOLO_NET_EVENT_TANK_KILL, TRUE, killer, killed,
                       botManagerIsBot(sim, killer), botManagerIsBot(sim, killed));
    logAddEvent(log_KillPlayer, killed, killer, 0, 0, 0, NULL);
    logAddEvent(log_PlayerDied, killed, 0, 0, 0, 0, NULL);
}

/* Append a packed attribution record to the per-round buffer. No-op unless a
 * round is running. Grows the buffer geometrically up to a hard cap; once the
 * cap (or an allocation failure) is hit, trackTruncated latches and further
 * records are dropped rather than crashing. */
void serverSimTrackAppend(ServerSim *sim, const void *rec, size_t n) {
    if (sim->state != serverStateRunning) return;
    if (sim->trackTruncated) return;
    if (sim->trackLen + n > ATTRIBUTION_TRACK_CAP_BYTES) { sim->trackTruncated = true; return; }
    if (sim->trackLen + n > sim->trackCap) {
        size_t newCap = sim->trackCap ? sim->trackCap * 2 : 4096;
        while (newCap < sim->trackLen + n) newCap *= 2;
        if (newCap > ATTRIBUTION_TRACK_CAP_BYTES) newCap = ATTRIBUTION_TRACK_CAP_BYTES;
        uint8_t *nb = (uint8_t *)realloc(sim->trackBuf, newCap);
        if (nb == NULL) { sim->trackTruncated = true; return; }  /* OOM: truncate, never crash */
        sim->trackBuf = nb; sim->trackCap = newCap;
    }
    memcpy(sim->trackBuf + sim->trackLen, rec, n);
    sim->trackLen += n;
    sim->trackRecordCount++;
}

void serverSimCbRecordDamage(void *ctx, BYTE attacker, BYTE targetKind,
                             BYTE targetIndex, BYTE source,
                             uint16_t dealt, bool destroyed,
                             BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    if (sim->state != serverStateRunning) return;
    /* Record every hit, including owner-less/NEUTRAL splash — the shared
     * derivation filters. The DMG_ and ATTR_ constants share numeric values,
     * so the target/source bytes copy across directly. */
    AttrDamageRecord r;
    r.type = ATTR_REC_DAMAGE; r.tick = sim->tick;
    r.source = source; r.target = targetKind; r.targetIndex = targetIndex;
    r.attacker = attacker; r.amount = dealt; r.destroyed = destroyed ? 1 : 0;
    r.mapX = mapX; r.mapY = mapY;
    serverSimTrackAppend(sim, &r, sizeof r);
    roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                          &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
}

void serverSimCbRecordPlayerAction(void *ctx, BYTE player, BYTE actionKind,
                                   BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    if (sim->state != serverStateRunning || player >= MAX_TANKS) return;
    AttrActionRecord r;
    r.type = ATTR_REC_ACTION; r.tick = sim->tick;
    r.player = player; r.action = actionKind;
    r.mapX = mapX; r.mapY = mapY;
    serverSimTrackAppend(sim, &r, sizeof r);
    roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                          &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
}

/* A tank scooped a dead (0-armour) pillbox into its inventory. Persisted to the
 * attribution track and appended to the notable timeline (no aggregate
 * counter); backs the pickup-spree highlight. Server-only; NULL on the client. */
void serverSimCbRecordPillPickup(void *ctx, BYTE picker, BYTE pillIndex,
                                 BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    if (sim->state != serverStateRunning) return;
    AttrPickupRecord r;
    r.type = ATTR_REC_PICKUP; r.tick = sim->tick;
    r.picker = picker; r.pillIndex = pillIndex;
    r.mapX = mapX; r.mapY = mapY;
    serverSimTrackAppend(sim, &r, sizeof r);
    roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                          &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
}

void serverSimCbCenterTank(void *ctx) {
    (void)ctx;
    /* No-op on server */
}

void serverSimCbConsoleMessage(void *ctx, char *msg) {
    ServerSim *sim = (ServerSim *)ctx;
    serverMessageConsoleMessage(sim, msg);
}

/* The three callbacks that answer rather than announce. Each is the one
 * place a decision the sim core takes is put to the scenario, which is why
 * the core can ask its question without a scenario existing: with no policy
 * registered, or none that has an opinion, the answer here is the classic
 * one and the caller carries on as it always did. Every call runs between
 * the policy enter and leave, so a policy that tries to write back through
 * the op funnel while it is answering is refused there. */

/* Where a tank starts. The index is the policy's to name and the caller's
 * to range-check. */
bool serverSimCbChooseStart(void *ctx, BYTE player, BYTE *startIdx) {
    ServerSim *sim = (ServerSim *)ctx;
    bool named;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->chooseStart == NULL) {
        return FALSE;
    }
    serverSimScenarioPolicyEnter(sim);
    named = sim->scenarioPolicy->chooseStart(sim->scenarioPolicy->ctx,
                                             player, startIdx);
    serverSimScenarioPolicyLeave(sim);
    return named;
}

/* What a spawning tank is handed. A loadout that names a game type is
 * turned into the four amounts here, so the sim core only ever deals in
 * amounts and the two forms of the answer cost it nothing. */
bool serverSimCbSpawnLoadout(void *ctx, BYTE player, BYTE *shells,
                             BYTE *mines, BYTE *armour, BYTE *trees) {
    ServerSim *sim = (ServerSim *)ctx;
    ScnLoadout wanted;
    bool answered;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->spawnLoadout == NULL) {
        return FALSE;
    }
    memset(&wanted, 0, sizeof(wanted));
    serverSimScenarioPolicyEnter(sim);
    answered = sim->scenarioPolicy->spawnLoadout(sim->scenarioPolicy->ctx,
                                                 player, &wanted);
    serverSimScenarioPolicyLeave(sim);
    if (answered == FALSE) {
        return FALSE;
    }
    if (wanted.useGameType != 0) {
        gameType named = (gameType)wanted.gameType;
        gameTypeGetItems(&sim->sim, &named, shells, mines, armour, trees);
        return TRUE;
    }
    *shells = wanted.shells;
    *mines  = wanted.mines;
    *armour = wanted.armour;
    *trees  = wanted.trees;
    return TRUE;
}

/* Whether a dead tank may come back yet. */
bool serverSimCbCanRespawn(void *ctx, BYTE player) {
    ServerSim *sim = (ServerSim *)ctx;
    bool may;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->canRespawn == NULL) {
        return TRUE;
    }
    serverSimScenarioPolicyEnter(sim);
    may = sim->scenarioPolicy->canRespawn(sim->scenarioPolicy->ctx, player);
    serverSimScenarioPolicyLeave(sim);
    return may;
}

/* The four combat questions. Same shape as the three above: the classic
 * answer with nothing registered, and the policy asked between the enter and
 * the leave so it cannot write back through the op funnel mid-question. */

/* What this blow is worth against this victim, as a percent. A hundred is
 * the classic amount and leaves the damage arithmetic exactly where it
 * was. */
int serverSimCbDamageScale(void *ctx, BYTE attacker, BYTE victim, BYTE cause) {
    ServerSim *sim = (ServerSim *)ctx;
    int pct;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->damageScale == NULL) {
        return 100;
    }
    serverSimScenarioPolicyEnter(sim);
    pct = sim->scenarioPolicy->damageScale(sim->scenarioPolicy->ctx, attacker,
                                           victim, cause);
    serverSimScenarioPolicyLeave(sim);
    return pct;
}

/* Whether a build order may go ahead. */
bool serverSimCbCanBuild(void *ctx, BYTE player, BYTE action, BYTE mapX,
                         BYTE mapY, BYTE pillIdx) {
    ServerSim *sim = (ServerSim *)ctx;
    bool may;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->canBuild == NULL) {
        return TRUE;
    }
    serverSimScenarioPolicyEnter(sim);
    may = sim->scenarioPolicy->canBuild(sim->scenarioPolicy->ctx, player,
                                        action, mapX, mapY, pillIdx);
    serverSimScenarioPolicyLeave(sim);
    return may;
}

/* Whether a pill or base may change hands. The engine's own capture tests
 * have already passed by the time this is asked. */
bool serverSimCbCanCapture(void *ctx, BYTE kind, BYTE index, BYTE player) {
    ServerSim *sim = (ServerSim *)ctx;
    bool may;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->canCapture == NULL) {
        return TRUE;
    }
    serverSimScenarioPolicyEnter(sim);
    may = sim->scenarioPolicy->canCapture(sim->scenarioPolicy->ctx, kind,
                                          index, player);
    serverSimScenarioPolicyLeave(sim);
    return may;
}

/* Whether this blow may destroy what it landed on. The kill ops do not come
 * through here: they call the death bodies directly, so a script that kills
 * what it protected gets the death it asked for. */
bool serverSimCbCanDie(void *ctx, BYTE kind, BYTE index, BYTE killer,
                       BYTE cause) {
    ServerSim *sim = (ServerSim *)ctx;
    bool may;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->canDie == NULL) {
        return TRUE;
    }
    serverSimScenarioPolicyEnter(sim);
    may = sim->scenarioPolicy->canDie(sim->scenarioPolicy->ctx, kind, index,
                                      killer, cause);
    serverSimScenarioPolicyLeave(sim);
    return may;
}
