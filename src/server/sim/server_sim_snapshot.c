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
 *Name:          Server Simulation Snapshot
 *Filename:      server_sim_snapshot.c
 *Author:        John Morrison
 *Purpose:
 *  The per-client view-building path. Each recipient gets a
 *  set of viewport rectangles — its tank screen plus its
 *  owned/allied pillbox screens — and the entity collectors
 *  below are culled against them. serverSimBuildSnapshot
 *  assembles the result into the snapshot buffer every
 *  transport hands to that subscriber.
 *********************************************************/

#include <string.h>

#include "server_sim_shared.h"
#include "server_sim_internal.h"
#include "../../common/wb_log.h"   /* WB_LOG_DEBUG — the one-shot clientFlags trace */

/* Viewport culling — margin in map squares beyond the visible 15×15 screen */
#define SNAPSHOT_SCREEN_SIZE 15
#define SNAPSHOT_VIEWPORT_MARGIN 20

bool inAnyViewport(const ViewportRect *vps, int count, int mx, int my) {
    int i;
    for (i = 0; i < count; i++) {
        if (mx >= vps[i].minMX && mx <= vps[i].maxMX &&
            my >= vps[i].minMY && my <= vps[i].maxMY) {
            return true;
        }
    }
    return false;
}

static int serverSimGetShells(ServerSim *sim, ShellSnapshot *out, int maxOut,
                              const ViewportRect *viewports, int numViewports) {
    shells q;
    int count = 0;

    q = sim->sim.shs;
    while (q != NULL && count < maxOut) {
        if (!inAnyViewport(viewports, numViewports, q->x >> 8, q->y >> 8)) {
            q = q->next;
            continue;
        }
        out[count].worldX = q->x;
        out[count].worldY = q->y;
        out[count].angle = (uint8_t)q->angle;
        out[count].owner = q->owner;
        out[count].length = q->length;
        count++;
        q = q->next;
    }
    return count;
}

static int serverSimGetTkExplosions(ServerSim *sim, TkExplosionSnapshot *out, int maxOut) {
    /* Tank fireballs are now replicated as one-shot EVENT_TK_EXPLOSION
     * reliable events at creation time (see serverSimCbTkExplosion). The
     * client simulates the trail locally, so we no longer send per-tick
     * snapshot data. */
    (void)sim; (void)out; (void)maxOut;
    return 0;
}

int serverSimGetBases(ServerSim *sim, BaseSnapshot *out, int maxOut) {
    int count = 0;
    BYTE nb;
    BYTE b;
    if (sim->sim.bs == NULL) return 0;
    nb = basesGetNumBases(&sim->sim.bs);
    for (b = 0; b < nb && count < maxOut; b++) {
        out[count].owner = (*sim->sim.bs).item[b].owner;
        out[count].armour = (*sim->sim.bs).item[b].armour;
        out[count].shells = (*sim->sim.bs).item[b].shells;
        out[count].mines = (*sim->sim.bs).item[b].mines;
        count++;
    }
    return count;
}

WORLD serverSimClosestBaseSendRange(const ServerSim *sim, BYTE client) {
    /* Widen the closest-base selection ceiling past the client's display range
     * (BASE_STATUS_RANGE) by roughly how far the tank travels in one round-trip
     * at max road speed (800 world units/sec): margin = ping_ms/1000 * 800,
     * plus 32 units fixed headroom. Clamp the margin to one map square (256) so
     * a spiking RTT can't widen the reveal past a single tile (ping effectively
     * capped at ~280ms); the total ceiling never exceeds BASE_STATUS_RANGE + 256.
     * Pre-loading the stock from this wider radius caches it before the client's
     * own display switches to the base, avoiding the stale "full health, 0 ammo"
     * flash. */
    unsigned margin = ((unsigned)sim->playerPing[client] * 800u) / 1000u + 32u;
    if (margin > 256u) {
        margin = 256u;
    }
    return (WORLD)(BASE_STATUS_RANGE + margin);
}

void serverSimBuildBaseStockEvent(ServerSim *sim, BYTE baseIdx0, GameEvent *out) {
    BYTE shells = 0, mines = 0, armour = 0;
    /* basesGetStats takes a 1-based base number; the wire index (data[0]) is 0-based. */
    basesGetStats(&sim->sim.bs, (BYTE)(baseIdx0 + 1), &shells, &mines, &armour);
    out->type = EVENT_BASE_STOCK;
    memset(out->data, 0, sizeof(out->data));
    out->data[0] = baseIdx0;
    out->data[1] = armour;
    out->data[2] = shells;
    out->data[3] = mines;
}

bool serverSimTakeClosestBaseStock(ServerSim *sim, BYTE recipient, BYTE closest, GameEvent *out) {
    bool changed = (closest != sim->lastClosestBase[recipient]);
    sim->lastClosestBase[recipient] = closest;
    if (!changed || closest == BASE_NOT_FOUND) {
        return false;
    }
    serverSimBuildBaseStockEvent(sim, (BYTE)(closest - 1), out);
    return true;
}

bool serverSimTakeArrivalBaseStock(ServerSim *sim, BYTE clientIdx, GameEvent *out) {
    WORLD wx = 0, wy = 0;
    BYTE closest = BASE_NOT_FOUND;
    /* Bots read base stock via the periodic full sync, not the arrival push —
     * matches the snapshot build's recipient-is-bot gate. */
    if (serverSimIsBot(sim, clientIdx)) {
        return false;
    }
    if (serverSimGetTankState(sim, clientIdx, &wx, &wy)) {
        WORLD r = serverSimClosestBaseSendRange(sim, clientIdx);
        closest = basesGetClosestForPlayer(&sim->sim, clientIdx, wx, wy, r);
    }
    return serverSimTakeClosestBaseStock(sim, clientIdx, closest, out);
}

int serverSimGetPills(ServerSim *sim, PillSnapshot *out, int maxOut) {
    int count = 0;
    BYTE np;
    BYTE p;
    if (sim->sim.pb == NULL) return 0;
    np = pillsGetNumPills(&sim->sim.pb);
    for (p = 0; p < np && count < maxOut; p++) {
        out[count].x = (*sim->sim.pb).item[p].x;
        out[count].y = (*sim->sim.pb).item[p].y;
        out[count].owner = (*sim->sim.pb).item[p].owner;
        out[count].armourInTank = pillPackArmourInTank((*sim->sim.pb).item[p].armour,
                                                       (*sim->sim.pb).item[p].inTank);
        count++;
    }
    return count;
}

int serverSimGetCompressedMap(ServerSim *sim, BYTE *output) {
    return mapSaveCompressedMap(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss, output);
}

int serverSimBuildViewports(ServerSim *sim, BYTE clientIdx, ViewportRect *out, int maxOut) {
    int n = 0;
    int halfView = (SNAPSHOT_SCREEN_SIZE / 2) + SNAPSHOT_VIEWPORT_MARGIN;
    WORLD clientWX = 0, clientWY = 0;
    if (n < maxOut && serverSimGetTankState(sim, clientIdx, &clientWX, &clientWY)) {
        int centerMX = clientWX >> 8;
        int centerMY = clientWY >> 8;
        out[n].minMX = centerMX - halfView; out[n].maxMX = centerMX + halfView;
        out[n].minMY = centerMY - halfView; out[n].maxMY = centerMY + halfView;
        n++;
    }
    if (sim->sim.pb != NULL) {
        BYTE np = pillsGetNumPills(&sim->sim.pb);
        BYTE p;
        for (p = 0; p < np && n < maxOut; p++) {
            BYTE owner = (*sim->sim.pb).item[p].owner;
            if (!playersIsAllie(&sim->sim.plyrs, owner, clientIdx)) continue;
            if ((*sim->sim.pb).item[p].inTank) continue;
            out[n].minMX = (*sim->sim.pb).item[p].x - halfView;
            out[n].maxMX = (*sim->sim.pb).item[p].x + halfView;
            out[n].minMY = (*sim->sim.pb).item[p].y - halfView;
            out[n].maxMY = (*sim->sim.pb).item[p].y + halfView;
            n++;
        }
    }
    if (n == 0) {
        out[0].minMX = 0; out[0].maxMX = 255;
        out[0].minMY = 0; out[0].maxMY = 255;
        n = 1;
    }
    return n;
}

void serverSimBuildSnapshot(ServerSim *sim, BYTE clientIdx,
                            SnapshotHeader *hdr,
                            TankSnapshot *tanksOut, int maxTanks,
                            ShellSnapshot *shellsOut, int maxShells,
                            TkExplosionSnapshot *tkExplOut, int maxTkExpl,
                            BaseSnapshot *basesOut, int maxBases,
                            PillSnapshot *pillsOut, int maxPills,
                            GameEvent *eventsOut, int maxEvents,
                            bool noCull) {
    int i;
    int tankCount = 0;
    ViewportRect viewports[MAX_VIEWPORTS];
    int numViewports = 0;

    memset(hdr, 0, sizeof(*hdr));
    hdr->serverTick = sim->tick;
    hdr->lastProcessedInput = sim->lastProcessedInput[clientIdx];
    /* Forced return-to-lobby countdown — clients render their own
     * "Returning to lobby in N" off this. 0 means no pending. */
    hdr->returnToLobbyTicks =
        (sim->returnToLobbyTicks > 0)
            ? (uint16_t)((sim->returnToLobbyTicks > 0xFFFF)
                          ? 0xFFFFu : sim->returnToLobbyTicks)
            : 0;

    /* Build the recipient's viewport set: under cull, the tank screen plus
     * owned/allied pillbox screens (with a full-map fallback); under noCull,
     * a single full-map viewport so everything is sent. */
    if (noCull) {
        viewports[0].minMX = 0; viewports[0].maxMX = 255;
        viewports[0].minMY = 0; viewports[0].maxMY = 255;
        numViewports = 1;
    } else {
        numViewports = serverSimBuildViewports(sim, clientIdx, viewports, MAX_VIEWPORTS);
    }

    /* Build tank snapshots for all connected players */
    for (i = 0; i < MAX_TANKS && tankCount < maxTanks; i++) {
        TankSnapshot *ts;
        WORLD wx, wy;

        if (!sim->playerConnected[i]) continue;
        if (!serverSimGetTankState(sim, (BYTE)i, &wx, &wy)) continue;

        /* Always include the client's own tank; cull others by viewport.
         * Also check LGM position — a parachuting LGM can be far from its
         * tank (starts at a random spawn), so we need to send updates when
         * the LGM is visible even if the tank is not.  Out-of-view tanks
         * are emitted as 1-byte stubs (TANK_SNAPSHOT_HIDDEN_FLAG) rather
         * than skipped, so the client can clear stale ghost positions for
         * tanks that have driven off screen.  noCull bypasses this so
         * recording paths capture every tank in full. */
        if (i != clientIdx && !noCull) {
            bool inView = inAnyViewport(viewports, numViewports, wx >> 8, wy >> 8);
            if (!inView && sim->sim.lgmen[i] != NULL && lgmIsOut(&sim->sim.lgmen[i])) {
                BYTE lgmMX = lgmGetMX(&sim->sim.lgmen[i]);
                BYTE lgmMY = lgmGetMY(&sim->sim.lgmen[i]);
                if (lgmMX != 0 || lgmMY != 0) {
                    inView = inAnyViewport(viewports, numViewports, lgmMX, lgmMY);
                }
            }
            if (!inView) {
                ts = &tanksOut[tankCount];
                memset(ts, 0, sizeof(*ts));
                ts->playerNum = (uint8_t)(i | TANK_SNAPSHOT_HIDDEN_FLAG);
                tankCount++;
                continue;
            }
        }

        ts = &tanksOut[tankCount];
        ts->playerNum = (uint8_t)i;
        ts->worldX = wx;
        ts->worldY = wy;
        ts->angle = (uint16_t)(tankGetAngle(&sim->sim.tanks[i]) * 256.0f);
        ts->speed = (uint16_t)(tankGetActualSpeed(&sim->sim.tanks[i]) * 256.0f);
        {
            BYTE onBoat = tankIsOnBoat(&sim->sim.tanks[i]) ? 1 : 0;
            BYTE isDead = (tankGetDeathWait(&sim->sim.tanks[i]) > 0) ? 1 : 0;
            ts->tankStatus = utilPutNibble(isDead, onBoat);
        }
        ts->lgmFrame = lgmIsOut(&sim->sim.lgmen[i]) ? (lgmGetFrame(&sim->sim.lgmen[i]) + 1) : 0;
        ts->lgmMX = lgmGetMX(&sim->sim.lgmen[i]);
        ts->lgmMY = lgmGetMY(&sim->sim.lgmen[i]);
        ts->lgmPX = lgmGetPX(&sim->sim.lgmen[i]);
        ts->lgmPY = lgmGetPY(&sim->sim.lgmen[i]);
        ts->firstLeft = tankGetFirstLeft(&sim->sim.tanks[i]);
        ts->firstRight = tankGetFirstRight(&sim->sim.tanks[i]);
        ts->pingMs = sim->playerPing[i];
        ts->clientFlags = playersGetClientFlags(&sim->sim.plyrs, (BYTE)i);
        { static bool _snaplg[16] = {0};
          if (!_snaplg[i] && ts->clientFlags != 0) {
            _snaplg[i] = 1;
            WB_LOG_DEBUG(WB_LOG_CAT_SERVER, "[WBN SNAP] player %d clientFlags=0x%02x", i, ts->clientFlags);
          }
        }

        /* Resources: only send to the owning player */
        if (i == clientIdx) {
            ts->armour = tankGetArmour(&sim->sim.tanks[i]);
            ts->shells = tankGetShells(&sim->sim.tanks[i]);
            ts->mines = tankGetMines(&sim->sim.tanks[i]);
            ts->trees = tankGetTrees(&sim->sim.tanks[i]);
            ts->gunsightLen = tankGetGunsightLength(&sim->sim.tanks[i]);
            ts->deathWait = (uint8_t)tankGetDeathWait(&sim->sim.tanks[i]);
            ts->reload = tankGetReloadTime(&sim->sim.tanks[i]);
        } else {
            ts->armour = 0;
            ts->shells = 0;
            ts->mines = 0;
            ts->trees = 0;
            ts->gunsightLen = 0;
            ts->deathWait = 0;
            ts->reload = 0;
        }
        tankCount++;
    }
    hdr->tankCount = (uint8_t)tankCount;

    /* Shell snapshots */
    hdr->shellCount = (uint8_t)serverSimGetShells(sim, shellsOut, maxShells,
                                                   viewports, numViewports);

    /* Tank explosion snapshots (globally important — no viewport filtering) */
    hdr->tkExplosionCount = (uint8_t)serverSimGetTkExplosions(sim, tkExplOut, maxTkExpl);

    /* Recipient-is-bot flag — bots are exempt from the base-stock visibility
     * cull (their brains read non-closest base armour for fog-of-war). */
    bool recipientIsBot = serverSimIsBot(sim, clientIdx);

    /* Periodic full base/pill/map sync to correct any client drift */
    if (sim->lastFullSyncTick[clientIdx] == 0 ||
        sim->tick - sim->lastFullSyncTick[clientIdx] >= FULL_SYNC_INTERVAL) {
        hdr->baseCount = (uint8_t)serverSimGetBases(sim, basesOut, maxBases);
        if (!recipientIsBot) {
            /* Per-recipient base visibility (owner is always real):
             *  - armour is public base condition: real for neutral/own/allied bases;
             *    an enemy base reports BASE_FULL_ARMOUR while alive (exact value hidden)
             *    but its true armour once dead/capturable, so the capturable flip shows.
             *    Mirrors the brain fog-of-war in basesGetBrainBaseInRect.
             *  - shells/mines are the private ammo reserve: real for every
             *    neutral/allied base, zeroed for enemy bases. Always sending a
             *    friendly base's stock means the client has it cached before its
             *    display ever switches to that base, so no stale 0/0 flash. */
            for (i = 0; i < hdr->baseCount; i++) {
                BYTE owner = basesOut[i].owner;
                bool friendly = (owner == NEUTRAL) || (owner == clientIdx) ||
                                playersIsAllie(&sim->sim.plyrs, owner, clientIdx);
                if (!friendly && basesOut[i].armour > MIN_ARMOUR_CAPTURE) {
                    basesOut[i].armour = BASE_FULL_ARMOUR;
                }
                if (!friendly) {
                    basesOut[i].shells = 0;
                    basesOut[i].mines  = 0;
                }
            }
        }
        hdr->pillCount = (uint8_t)serverSimGetPills(sim, pillsOut, maxPills);
        hdr->mapChecksum = mapCalcChecksum(&sim->sim.mp, &sim->sim.bs, &sim->sim.pb);
        sim->lastFullSyncTick[clientIdx] = sim->tick;
    } else {
        hdr->baseCount = 0;
        hdr->pillCount = 0;
        hdr->mapChecksum = 0;
    }

    /* Game events — filter EVENT_SOUND by distance and deduplicate per type.
     * Non-sound events pass through unchanged. */
    {
        int outCount = 0;
        WORLD cwx = 0, cwy = 0;
        BYTE clientMX = 0, clientMY = 0;
        bool hasClientPos = serverSimGetTankState(sim, clientIdx, &cwx, &cwy);
        if (hasClientPos) {
            clientMX = (BYTE)(cwx >> 8);
            clientMY = (BYTE)(cwy >> 8);
        }

        /* First pass: collect best (closest) sound event per sound type.
         * Track by soundId index — sndEffects has ~24 values. */
        #define MAX_SOUND_TYPES 32
        int bestSoundIdx[MAX_SOUND_TYPES];   /* index into sim->events */
        int bestSoundDist[MAX_SOUND_TYPES];  /* manhattan distance to client */
        int s;
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            bestSoundIdx[s] = -1;
            bestSoundDist[s] = 255;
        }

        for (i = 0; i < sim->eventCount; i++) {
            uint8_t evType = sim->events[i].type;
            if (evType == EVENT_SOUND || evType == EVENT_SOUND_TANK_HIT || evType == EVENT_SOUND_SHOOT) {
                uint8_t soundId = sim->events[i].data[0];
                uint8_t mx = sim->events[i].data[1];
                uint8_t my = sim->events[i].data[2];

                if (!hasClientPos) continue;

                /* Skip own shoot sound — client plays shootSelf via prediction */
                if (evType == EVENT_SOUND_SHOOT && sim->events[i].data[3] == clientIdx) {
                    continue;
                }

                /* Bubbles only go to the player losing ammo in water */
                if (evType == EVENT_SOUND && soundId == bubbles && sim->events[i].data[3] != clientIdx) {
                    continue;
                }

                /* Calculate manhattan distance to client */
                int dx = (clientMX > mx) ? (clientMX - mx) : (mx - clientMX);
                int dy = (clientMY > my) ? (clientMY - my) : (my - clientMY);

                /* Always send tank hits to the hit player (plays hitTankSelf at full volume) */
                if (evType == EVENT_SOUND_TANK_HIT && sim->events[i].data[3] == clientIdx) {
                    /* Skip distance cull */
                } else if (dx >= SDIST_NONE || dy >= SDIST_NONE) {
                    continue;
                }

                /* Keep closest instance of each sound type */
                int dist = dx + dy;
                if (soundId < MAX_SOUND_TYPES && dist < bestSoundDist[soundId]) {
                    bestSoundIdx[soundId] = i;
                    bestSoundDist[soundId] = dist;
                }
            }
        }

        /* Copy map events first (from dedicated buffer), then non-sound
         * events, then deduplicated sound events */
        for (i = 0; i < sim->mapEventCount && outCount < maxEvents; i++) {
            eventsOut[outCount++] = sim->mapEvents[i];
        }
        for (i = 0; i < sim->eventCount && outCount < maxEvents; i++) {
            uint8_t evType = sim->events[i].type;
            if (evType != EVENT_SOUND && evType != EVENT_SOUND_TANK_HIT && evType != EVENT_SOUND_SHOOT) {
                /* Filter EVENT_MINE_VISIBLE: tank mines (bit 7 set) go to all,
                 * LGM mines go only to the placer and their allies */
                if (evType == EVENT_MINE_VISIBLE) {
                    BYTE sp = sim->events[i].data[2];
                    if (!(sp & 0x80) && clientIdx != (sp & 0x7F) &&
                        !playersIsAllie(&sim->sim.plyrs, clientIdx, sp)) {
                        continue;
                    }
                }
                /* Viewport-cull explosion events */
                if (evType == EVENT_EXPLOSION) {
                    if (!inAnyViewport(viewports, numViewports, sim->events[i].data[0], sim->events[i].data[1])) {
                        continue;
                    }
                }
                /* Cull base stock to neutral/allied bases (humans only — bots
                 * receive every event). Enemy-base ammo stays hidden. */
                if (evType == EVENT_BASE_STOCK && !recipientIsBot) {
                    BYTE bIdx = sim->events[i].data[0];
                    BYTE bOwner = (*sim->sim.bs).item[bIdx].owner;
                    bool bFriendly = (bOwner == NEUTRAL) || (bOwner == clientIdx) ||
                                     playersIsAllie(&sim->sim.plyrs, bOwner, clientIdx);
                    if (!bFriendly) {
                        continue;
                    }
                }
                eventsOut[outCount++] = sim->events[i];
            }
        }
        for (s = 0; s < MAX_SOUND_TYPES && outCount < maxEvents; s++) {
            if (bestSoundIdx[s] >= 0) {
                eventsOut[outCount++] = sim->events[bestSoundIdx[s]];
            }
        }
        #undef MAX_SOUND_TYPES

        /* The arrival base-stock push is no longer emitted here: it writes
         * per-client sim state (lastClosestBase) that the first caller each
         * tick consumed, starving the others, and the UDP path discards this
         * build's events anyway. Each delivery path now owns the push — the
         * UDP path in transportUdpServerDrainEvents, the local transport via
         * serverSimTakeArrivalBaseStock — so lastClosestBase has one consumer
         * per client and the push is never stolen. */

        hdr->reliableEventCount = (uint8_t)outCount;
    }
}
