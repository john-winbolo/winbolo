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
#include <stdio.h>                /* snprintf — the three-shot order line */

#include "server_sim_shared.h"
#include "bolo_map.h"             /* mapGetPos / mapPosInBounds — the ordered square */
#include "../../common/wb_log.h"  /* WB_LOG_INFO — the three-shot order trace */
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

/* ── THREE SHOTS = GO THERE ──────────────────────────────────────────
 *
 * Which squares an order may be dropped on. The rule is "open ground a
 * tank could be told to stand on", and it is deliberately generous with
 * water because the design lists it:
 *
 *   counts  — GRASS, ROAD, SWAMP, CRATER, RUBBLE, RIVER, BOAT (shallow
 *             water) and DEEP_SEA
 *   does not — FOREST, BUILDING (the wall), HALFBUILDING, and any square
 *             holding a pillbox or a base
 *
 * A mined square is judged by what is under the mine: MINE_GRASS counts
 * because GRASS does, MINE_FOREST does not because FOREST does not. The
 * mine terrain codes run MINE_START..MINE_END and sit MINE_SUBTRACT above
 * their own base type.
 *
 * The pill and base tests are belt and braces: a shell that hits either
 * resolves in shellsCalcCollision and never reaches the expiry path. A
 * DEAD pillbox is flown over rather than hit, though, and an order on top
 * of one is not "open ground" in any sense a player means. */
static bool shotOrderOpenSquare(GameSim *gs, BYTE mx, BYTE my) {
    BYTE terrain;

    /* Off the map FIRST. mapGetPos answers DEEP_SEA for a square the map
     * does not hold, and deep sea is in the open list below — so without
     * this the whole border reads as open ground and an order could be put
     * on a square no tank can ever stand on. */
    if (!mapPosInBounds(mx, my)) return false;

    terrain = mapGetPos(&gs->mp, mx, my);

    if (terrain >= MINE_START && terrain <= MINE_END) {
        terrain = (BYTE)(terrain - MINE_SUBTRACT);
    }
    switch (terrain) {
        case GRASS:
        case ROAD:
        case SWAMP:
        case CRATER:
        case RUBBLE:
        case RIVER:
        case BOAT:
        case DEEP_SEA:
            break;
        default:                 /* FOREST, BUILDING, HALFBUILDING */
            return false;
    }
    if (pillsExistPos(&gs->pb, mx, my)) return false;
    if (basesExistPos(&gs->bs, mx, my)) return false;
    return true;
}

void serverSimShotOrderClear(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    memset(&sim->shotOrder[playerNum], 0, sizeof(sim->shotOrder[playerNum]));
}

/* Did this player fire a shell on any tick from `lo` to `hi`, both ends
 * counted? The log is small and unsorted, so it is read straight through. */
static bool shotOrderFiredBetween(const ShotOrderRing *ring,
                                  uint32_t lo, uint32_t hi) {
    uint8_t i;
    if (lo > hi) return false;
    for (i = 0; i < ring->fireCount; i++) {
        if (ring->fire[i] >= lo && ring->fire[i] <= hi) return true;
    }
    return false;
}

/* The quiet second AFTER the third shot: nothing fired from the tick after
 * it up to and including SHOT_ORDER_QUIET_TICKS later. */
static bool shotOrderAfterIsQuiet(const ShotOrderRing *ring) {
    return !shotOrderFiredBetween(ring, ring->armFireTick + 1,
                                  ring->armFireTick +
                                      (uint32_t)SHOT_ORDER_QUIET_TICKS);
}

/* Put the order on the wire. The shooter's own tile goes in it so a bot can
 * answer the one question the server cannot: whether the shooter could see
 * it. The tile is read HERE, when the order is sent, which is the moment the
 * range rule is about.
 *
 * The brains read this as a chat line. The leading "!" is the forced form
 * their parser wants, which is also what keeps a bot's own chatter from
 * being read as an order. The sender is the SHOOTER, so the brain's own team
 * check (the sender's bit in info.allies) decides who obeys;
 * botManagerDeliverInternalMessage hands it to the shooter's allied bots and
 * skips the shooter's own slot.
 *
 * Deliver rather than Queue: this runs on the producer thread inside the sim
 * tick, and the queue is drained per bot slot for messages a bot's own
 * worker produced — a human shooter has no worker to drain it, so a queued
 * line would sit there for ever. */
static void shotOrderSend(ServerSim *sim, BYTE owner, BYTE mx, BYTE my) {
    char  msg[48];
    WORLD swx = 0, swy = 0;
    BYTE  sx = mx, sy = my;

    /* A shooter with no tank left is the one case with nowhere to read a
     * view from; the ordered square stands in for it, so the bots near the
     * square still take the order rather than none of them bidding. */
    if (sim->sim.tanks[owner] != NULL) {
        tankGetWorld(&sim->sim.tanks[owner], &swx, &swy);
        sx = (BYTE)(swx >> TANK_SHIFT_MAPSIZE);
        sy = (BYTE)(swy >> TANK_SHIFT_MAPSIZE);
    }

    snprintf(msg, sizeof(msg), "!goto %u %u %u %u",
             (unsigned)mx, (unsigned)my, (unsigned)sx, (unsigned)sy);
    botManagerDeliverInternalMessage(sim, owner, msg);
    WB_LOG_INFO(WB_LOG_CAT_SERVER,
                "three shots from p%u on (%u,%u) -> \"%s\"",
                (unsigned)owner, (unsigned)mx, (unsigned)my, msg);
}

void serverSimShotOrderShotFired(ServerSim *sim, BYTE owner,
                                 uint32_t fireTick) {
    ShotOrderRing *ring;
    uint8_t        i;

    if (sim == NULL || owner >= MAX_TANKS) return;
    ring = &sim->shotOrder[owner];

    /* A shell is heard of once at most, but the log is cheap to check and a
     * second copy of one fire tick would only waste a slot. */
    for (i = 0; i < ring->fireCount; i++) {
        if (ring->fire[i] == fireTick) return;
    }

    ring->fire[ring->fireNext] = fireTick;
    ring->fireNext = (uint8_t)((ring->fireNext + 1) % SHOT_ORDER_FIRE_LOG);
    if (ring->fireCount < SHOT_ORDER_FIRE_LOG) {
        ring->fireCount++;
    }

    /* A shot inside the quiet second after the third takes the armed order
     * away. The shot is not wasted: it lands like any other, so it can be
     * the first of a fresh run. */
    if (ring->armed && fireTick > ring->armFireTick &&
        fireTick - ring->armFireTick <= (uint32_t)SHOT_ORDER_QUIET_TICKS) {
        ring->armed = false;
    }
}

void serverSimShotOrderNote(ServerSim *sim, BYTE owner, uint32_t fireTick,
                            WORLD wx, WORLD wy) {
    GameSim       *gs;
    ShotOrderRing *ring;
    BYTE           mx, my;
    uint32_t       first, third, lo;
    int            i;

    if (sim == NULL || owner >= MAX_TANKS) return;
    gs = &sim->sim;
    mx = (BYTE)(wx >> TANK_SHIFT_MAPSIZE);
    my = (BYTE)(wy >> TANK_SHIFT_MAPSIZE);

    /* A square an order cannot be dropped on is not recorded at all — it
     * neither counts nor breaks a run. Three shells on one square inside
     * the window are the whole test; a fourth shell somewhere else in the
     * middle of them does not make the three less deliberate. */
    if (!shotOrderOpenSquare(gs, mx, my)) return;

    ring = &sim->shotOrder[owner];
    for (i = 0; i < SHOT_ORDER_SHOTS - 1; i++) {
        ring->mx[i]   = ring->mx[i + 1];
        ring->my[i]   = ring->my[i + 1];
        ring->tick[i] = ring->tick[i + 1];
    }
    ring->mx[SHOT_ORDER_SHOTS - 1]   = mx;
    ring->my[SHOT_ORDER_SHOTS - 1]   = my;
    ring->tick[SHOT_ORDER_SHOTS - 1] = fireTick;
    if (ring->count < SHOT_ORDER_SHOTS) {
        ring->count++;
    }
    if (ring->count < SHOT_ORDER_SHOTS) {
        return;                       /* fewer than three so far */
    }

    /* All three on one square... */
    for (i = 0; i < SHOT_ORDER_SHOTS - 1; i++) {
        if (ring->mx[i] != mx || ring->my[i] != my) return;
    }

    /* ...fired inside the window. The three are held oldest FIRST by
     * landing, and a shell fired later can land earlier when the ranges
     * differ, so a run whose fire ticks go backwards is not three shots at
     * one square in a row and is left to the next landing to sort out. */
    first = ring->tick[0];
    third = ring->tick[SHOT_ORDER_SHOTS - 1];
    if (third < first) return;
    if (third - first > (uint32_t)SHOT_ORDER_WINDOW_TICKS) return;

    /* ...with a quiet second in front of the first of them. The player's own
     * three are the only shells allowed anywhere near this burst. */
    lo = (first > (uint32_t)SHOT_ORDER_QUIET_TICKS)
             ? first - (uint32_t)SHOT_ORDER_QUIET_TICKS
             : 0;
    if (first > 0 && shotOrderFiredBetween(ring, lo, first - 1)) return;

    /* The three are spent either way, so they cannot order twice; the fire
     * log and any armed order stay, because the quiet second after this one
     * is still being served. */
    memset(ring->mx, 0, sizeof(ring->mx));
    memset(ring->my, 0, sizeof(ring->my));
    memset(ring->tick, 0, sizeof(ring->tick));
    ring->count = 0;

    /* ARMED, not sent. Whether the second after the third shot stays quiet
     * cannot be known yet, so serverSimShotOrderTick sends it when that
     * second is up and serverSimShotOrderShotFired takes it away if the
     * player shoots again first. */
    ring->armed       = true;
    ring->armMx       = mx;
    ring->armMy       = my;
    ring->armFireTick = third;
}

void serverSimShotOrderTick(ServerSim *sim) {
    BYTE p;

    if (sim == NULL) return;
    for (p = 0; p < MAX_TANKS; p++) {
        ShotOrderRing *ring = &sim->shotOrder[p];
        if (!ring->armed) continue;
        /* The quiet second is measured from the FIRE, so a shell that was a
         * long time in the air can leave the second already up when the
         * order is armed. It then goes out on the next tick, which is what
         * "as soon as the second is quiet" means. */
        if (sim->tick < ring->armFireTick + (uint32_t)SHOT_ORDER_QUIET_TICKS) {
            continue;
        }
        ring->armed = false;
        if (!shotOrderAfterIsQuiet(ring)) continue;
        shotOrderSend(sim, p, ring->armMx, ring->armMy);
    }
}

/* A shell owned by `owner` LEFT THE GUN. shellsAddItem calls this the moment
 * the shell is created, which is the moment the three-shot detector's rules
 * are all measured from, and takes back the tick it is recorded on.
 *
 * THE SERVER'S OWN TICK, never the client's. The shell also carries the
 * client's input-tick counter (shells.h fireTick), and that number is the
 * client's to choose: a mid-round joiner's counter starts near zero, so its
 * "three shots" would sit a thousand ticks behind the server and the quiet
 * second either side would be read against ticks that have long gone by;
 * a modified client could name any tick it liked. sim->tick is a number
 * nobody outside the server writes, so it is the one every rule is on.
 *
 * Before the fire log was fed here it was fed by serverSimCbShellDeath
 * alone, so the server heard of a shell only when it died — 104 ticks late
 * — and two shells went missing from the log at exactly the wrong moment:
 * one fired inside the quiet second after the third shot but still in the
 * air when the poll sent the order, and one fired just before the first of
 * the three whose longer flight kept it out of the log when the third
 * landed. Both now count.
 *
 * A pillbox fires with owner NEUTRAL (0xFF), which
 * serverSimShotOrderShotFired drops on its own; the tick is still answered,
 * so the pill's shell carries one like any other. */
uint32_t serverSimCbShellFired(void *ctx, BYTE owner) {
    ServerSim *sim = (ServerSim *)ctx;
    if (sim == NULL) return 0;
    serverSimShotOrderShotFired(sim, owner, sim->tick);
    return sim->tick;
}

/* A shell owned by `owner` ended (collision or expiry). Publish a
 * unicast CTRL_SHELL_DEATH so the firing client can match fireTick to
 * its predicted shell, cull the ghost, and draw the impact at
 * (impactWX, impactWY). udpClientDeliverControl filters to the owner.
 *
 * fireTick goes on the wire and nowhere else: it is the CLIENT's counter
 * and only that client can match it. serverFireTick is the server's own
 * tick at the moment the shell was created, which is the one the detector
 * below is given — see serverSimCbShellFired. */
void serverSimCbShellDeath(void *ctx, uint32_t fireTick,
                           uint32_t serverFireTick, BYTE owner,
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

    /* Every shell counts as a shot FIRED, whatever it hit: the quiet second
     * either side of the three asks what the player pulled the trigger on,
     * and a shell that struck a wall was still a trigger pull. A repeat of
     * the call serverSimCbShellFired already made at creation, which the
     * fire log ignores because that server tick is in it already — it is
     * kept so a shell that reached the world by some other road than
     * shellsAddItem still counts. */
    serverSimShotOrderShotFired(sim, owner, serverFireTick);

    /* Only a shell that ran its full range with nothing hit can be an
     * order. Every hit — pill, tank, base, wall, building, forest —
     * resolves in shellsCalcCollision and reports another outcome, which
     * is what keeps a normal firefight from ordering anybody about. */
    if (outcome == SHELL_OUTCOME_EXPIRED) {
        serverSimShotOrderNote(sim, owner, serverFireTick, impactWX, impactWY);
    }
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
    /* The pickup is also published. pillIndex reaches this callback counted
       from one — tankTakePill's argument is the pillbox number, not the array
       position — while every index on an event is the 0-based item[] slot, so
       the event subtracts one. The record below keeps the number it has
       always kept; moving it would rewrite what old recordings mean. */
    if (pillIndex > 0) {
        GameEvent ev;
        ev.type = EVENT_PILL_PICKED_UP;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = picker;
        ev.data[1] = (BYTE)(pillIndex - 1);
        serverSimAddEvent(sim, &ev);
    }
    if (sim->state != serverStateRunning) return;
    AttrPickupRecord r;
    r.type = ATTR_REC_PICKUP; r.tick = sim->tick;
    r.picker = picker; r.pillIndex = pillIndex;
    r.mapX = mapX; r.mapY = mapY;
    serverSimTrackAppend(sim, &r, sizeof r);
    roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                          &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
}

/* A base changed hands. The sim core settled the change and worked out which
 * of the three kinds it was; turning that into the event the snapshot stream
 * carries is the server's job and happens here. newOwner is NEUTRAL when the
 * base was neutralised rather than taken.
 *
 * data[0] to data[3] are the wire payload — gameEventDataSize() gives
 * EVENT_BASE_CAPTURED four bytes, so a client learns which base as well as who
 * holds it. data[3] is the quiet byte the announce policy answers with. From
 * data[4] on is the server-internal side channel the stats funnel in
 * serverSimAddEvent reads: the capture class and the base's map cell. */
void serverSimCbBaseOwnerChanged(void *ctx, BYTE index, BYTE oldOwner,
                                 BYTE newOwner, BYTE captureClass,
                                 BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_BASE_CAPTURED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = newOwner;
    ev.data[1] = oldOwner;
    ev.data[2] = index;
    ev.data[3] = serverSimAnnounce(sim, ANNOUNCE_KIND_BASE_CAPTURED, index,
                                   newOwner) ? 0 : 1;
    ev.data[4] = captureClass;
    ev.data[5] = mapX;
    ev.data[6] = mapY;
    serverSimAddEvent(sim, &ev);
}

/* A pillbox changed hands. The same shape as the base above, and the same
 * split between the four wire bytes and the three the funnel reads. */
void serverSimCbPillOwnerChanged(void *ctx, BYTE index, BYTE oldOwner,
                                 BYTE newOwner, BYTE captureClass,
                                 BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_PILL_CAPTURED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = newOwner;
    ev.data[1] = oldOwner;
    ev.data[2] = index;
    ev.data[3] = serverSimAnnounce(sim, ANNOUNCE_KIND_PILL_CAPTURED, index,
                                   newOwner) ? 0 : 1;
    ev.data[4] = captureClass;
    ev.data[5] = mapX;
    ev.data[6] = mapY;
    serverSimAddEvent(sim, &ev);
}

/* A builder was lost. data[0] to data[2] are the three wire bytes, the third
 * being the quiet byte the announce policy answers with; data[3] and data[4]
 * are the man's map cell, past gameEventDataSize() and read by the stats
 * funnel for the LGM record's mapX/mapY. */
void serverSimCbLgmDied(void *ctx, BYTE victim, BYTE killer,
                        BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_LGM_LOST;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = victim;
    ev.data[1] = killer;
    ev.data[2] = serverSimAnnounce(sim, ANNOUNCE_KIND_BUILDER_LOST, victim,
                                   killer) ? 0 : 1;
    ev.data[3] = mapX;
    ev.data[4] = mapY;
    serverSimAddEvent(sim, &ev);
}

/* The facts that had no event at all before. Each casts ctx once and builds
 * the event its gameEventDataSize row describes; nothing here reads the sim
 * beyond the queue, because the call site has already worked the values out. */

void serverSimCbTankSpawned(void *ctx, BYTE player, BYTE mapX, BYTE mapY,
                            bool respawn) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_TANK_SPAWNED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = player;
    ev.data[1] = mapX;
    ev.data[2] = mapY;
    ev.data[3] = respawn ? 1 : 0;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbLgmLanded(void *ctx, BYTE player, BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_LGM_LANDED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = player;
    ev.data[1] = mapX;
    ev.data[2] = mapY;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbPillPlaced(void *ctx, BYTE player, BYTE index, BYTE mapX,
                           BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_PILL_PLACED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = player;
    ev.data[1] = index;
    ev.data[2] = mapX;
    ev.data[3] = mapY;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbPillKilled(void *ctx, BYTE index, BYTE attacker) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_PILL_KILLED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = index;
    ev.data[1] = attacker;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbBuilt(void *ctx, BYTE player, BYTE action, BYTE mapX,
                      BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_BUILT;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = player;
    ev.data[1] = action;
    ev.data[2] = mapX;
    ev.data[3] = mapY;
    serverSimAddEvent(sim, &ev);
}

/* The one event that must not be serialized. It goes on the tick's queue like
 * any other, so the host's subscriber and the god-view recording both see it;
 * gameEventIsLocal is what keeps it out of every client's snapshot and off the
 * UDP drain. */
void serverSimCbMineLaid(void *ctx, BYTE player, BYTE mapX, BYTE mapY) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_MINE_PLACED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = player;
    ev.data[1] = mapX;
    ev.data[2] = mapY;
    serverSimAddEvent(sim, &ev);
}

void serverSimCbMineExploded(void *ctx, BYTE mapX, BYTE mapY, BYTE layer) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_MINE_EXPLODED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = mapX;
    ev.data[1] = mapY;
    /* The layer sits behind the wire bytes (gameEventDataSize is 2), so an
       in-process subscriber reads it and a remote client does not: the wire
       has never said whose minefield a square belongs to, and a client that
       could read it back by shelling squares would learn hidden mines'
       owners. */
    ev.data[2] = layer;
    serverSimAddEvent(sim, &ev);
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

    /* A loadout the spawn op named for this tank outranks the policy, and is
       answered before the policy is even looked for: a script that spawns a
       bot with a named loadout and writes no spawn_loadout function is the
       ordinary case, and the early return below would drop the answer. Taken
       as it is read, so it fuels this tank and not the seat's next one. */
    if (player < MAX_TANKS && sim->sim.scenarioSpawnLoadout[player] != 0) {
        gameType named = (gameType)sim->sim.scenarioSpawnLoadout[player];
        sim->sim.scenarioSpawnLoadout[player] = 0;
        gameTypeGetItems(&sim->sim, &named, shells, mines, armour, trees);
        return TRUE;
    }

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

/* Whether this fact may be shown to players. Every site that builds a
 * newswire-worthy fact comes through here, so the enter and leave bracket is
 * written once and a policy that answers by writing back through the op funnel
 * is refused there. A ServerSim rather than a void *ctx: the askers are server
 * sources holding the sim, not GameSim callbacks. */
bool serverSimAnnounce(ServerSim *sim, BYTE kind, BYTE subject, BYTE actor) {
    bool show;

    if (sim == NULL || sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->announce == NULL) {
        return TRUE;
    }
    serverSimScenarioPolicyEnter(sim);
    show = sim->scenarioPolicy->announce(sim->scenarioPolicy->ctx, kind,
                                         subject, actor);
    serverSimScenarioPolicyLeave(sim);
    return show;
}
