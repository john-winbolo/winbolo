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
 *  set of viewport rectangles — its tank screen plus the
 *  allied pillbox, base and tank screens its view policies
 *  allow — and the entity collectors
 *  below are culled against them. serverSimBuildSnapshot
 *  assembles the result into the snapshot buffer every
 *  transport hands to that subscriber.
 *********************************************************/

#include <string.h>

#include "server_sim_shared.h"
#include "server_sim_internal.h"
#include "../../common/wb_log.h"   /* WB_LOG_DEBUG — the one-shot clientFlags trace */

/* Viewport culling — margin in map squares beyond the visible 15×15 screen.
 * Every rect reaches SNAPSHOT_SCREEN_SIZE / 2 + SNAPSHOT_VIEWPORT_MARGIN
 * squares from its centre, so the margin below puts the half-extent at 19 and
 * each rect covers 39x39 squares.
 *
 * 19 is the overview's tank reveal block plus room for travel. The reveal block
 * is OVERVIEW_TANK_HALF, which is 14 (overview_types.h): a client cannot draw
 * ground it was never sent, so the cull can never be tighter than what the
 * overview reveals. The main screen's own edge only reaches 13 squares from the
 * tank under full autoscroll lead, so it is the reveal that sets the floor, not
 * the screen.
 *
 * The five squares on top cover travel and interpolation slack. A tank's top
 * speed is 3.125 map squares a second (MAP_SPEED_TROAD is 16 world units a
 * move, at 50 moves a second, with 256 units to a square), so a 400 ms round
 * trip is 1.25 squares — a tank driving at an edge was sent the ground ahead of
 * it well before it gets there.
 *
 * 14 + 5 = 19, written as a margin on the screen half because that is the shape
 * the rect builder wants: 19 - 7 = 12. */
#define SNAPSHOT_SCREEN_SIZE 15
#define SNAPSHOT_VIEWPORT_MARGIN 12

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

/* Written once here and exported through server_sim_internal.h so both sound
 * delivery paths measure the same way.
 *
 * The tier follows the general near/far rule the client applies today: near
 * inside the SDIST_SOFT square on both axes, far outside it. The bearing is
 * map-absolute, with map Y increasing southward, and is a pure axis when one
 * component is more than twice the other, a diagonal otherwise.
 *
 * The listener square comes from wx >> 8, which every other server-side cull
 * uses. That sits half a square from the client's own tankGetScreenMX, so a
 * boundary case can land one square either side of what the client would work
 * out for itself. */
bool soundTierAndDirection(int listenerMX, int listenerMY, int mx, int my,
                           uint8_t *tier, uint8_t *dir) {
    int dx = mx - listenerMX;
    int dy = my - listenerMY;
    int ax = (dx < 0) ? -dx : dx;
    int ay = (dy < 0) ? -dy : dy;

    *tier = (ax <= SDIST_SOFT && ay <= SDIST_SOFT) ? SOUND_TIER_NEAR
                                                   : SOUND_TIER_FAR;

    if (dx == 0 && dy == 0) {
        *dir = SOUND_DIR_CENTRE;
    } else if (ay > 2 * ax) {
        *dir = (dy < 0) ? SOUND_DIR_N : SOUND_DIR_S;
    } else if (ax > 2 * ay) {
        *dir = (dx > 0) ? SOUND_DIR_E : SOUND_DIR_W;
    } else if (dx > 0) {
        *dir = (dy < 0) ? SOUND_DIR_NE : SOUND_DIR_SE;
    } else {
        *dir = (dy < 0) ? SOUND_DIR_NW : SOUND_DIR_SW;
    }

    return ax < SDIST_NONE && ay < SDIST_NONE;
}

bool soundEventIsSound(uint8_t type) {
    return type == EVENT_SOUND || type == EVENT_SOUND_TANK_HIT ||
           type == EVENT_SOUND_SHOOT;
}

void soundPickInit(SoundPick *pick) {
    int s;
    for (s = 0; s < SOUND_PICK_TYPES; s++) {
        pick->has[s] = false;
        pick->dist[s] = 0;
    }
}

void soundPickOffer(SoundPick *pick, const GameEvent *ev, BYTE recipient,
                    int listenerMX, int listenerMY, bool keepSquare) {
    uint8_t soundId = ev->data[0];
    int mx = ev->data[1];
    int my = ev->data[2];
    uint8_t tier = SOUND_TIER_NEAR;
    uint8_t dir = SOUND_DIR_CENTRE;
    bool inRange;
    int ax, ay, dist;

    if (!soundEventIsSound(ev->type) || soundId >= SOUND_PICK_TYPES) return;

    /* Own shot: the client plays shootSelf from its prediction. */
    if (ev->type == EVENT_SOUND_SHOOT && ev->data[3] == recipient) return;

    /* Bubbles and a tank going under are each tied to one player's own boat
     * or drowning, so they only go to that player. */
    if (ev->type == EVENT_SOUND &&
        (soundId == bubbles || soundId == tankSinkNear ||
         soundId == tankSinkFar) &&
        ev->data[3] != recipient) {
        return;
    }

    /* Worked out for every sound, including the tank hit below that skips
     * the range cull, so the winner always has a tier and a bearing. */
    inRange = soundTierAndDirection(listenerMX, listenerMY, mx, my,
                                    &tier, &dir);

    /* A tank hit reaches the player hit at any range: they play hitTankSelf
     * at full volume. Everything else stops at SDIST_NONE. */
    if (!inRange &&
        !(ev->type == EVENT_SOUND_TANK_HIT && ev->data[3] == recipient)) {
        return;
    }

    /* Neither bubbles nor manLayingMineNear has a far variant, so a far one
     * is silence at a recipient that plays tiers. Dropped ahead of the dedup
     * so it cannot take the slot a nearer one wants. A recipient that keeps
     * the square reads the sound as a position, not a variant, and gets it. */
    if (!keepSquare && tier == SOUND_TIER_FAR && ev->type == EVENT_SOUND &&
        (soundId == bubbles || soundId == manLayingMineNear)) {
        return;
    }

    /* Closest instance of each sound id wins, measured on the real squares. */
    ax = (mx > listenerMX) ? (mx - listenerMX) : (listenerMX - mx);
    ay = (my > listenerMY) ? (my - listenerMY) : (listenerMY - my);
    dist = ax + ay;
    if (pick->has[soundId] && dist >= pick->dist[soundId]) return;

    /* Shaped into the pick's own copy: the sim's event array is shared by
     * every recipient in the tick, so rewriting it in place would hand the
     * next client a bearing measured against this one's tank. */
    pick->ev[soundId] = *ev;
    if (!keepSquare) {
        pick->ev[soundId].data[1] = tier;
        pick->ev[soundId].data[2] = dir;
    }
    pick->dist[soundId] = dist;
    pick->has[soundId] = true;
}

void serverSimSetSoundSquares(ServerSim *sim, BYTE playerNum, bool keep) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    sim->soundSquares[playerNum] = keep;
}

bool serverSimRecipientKeepsSoundSquares(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return false;
    return serverSimIsBot(sim, playerNum) || sim->soundSquares[playerNum];
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

int serverSimGetCompressedMap(ServerSim *sim, BYTE *output, int outputCap) {
    return mapSaveCompressedMap(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss,
                                output, outputCap);
}

void serverSimShadowSeed(ServerSim *sim, BYTE slot) {
    if (sim == NULL || slot >= MAX_TANKS) return;
    /* Bind the handle here rather than at create: serverSimInit memsets the
     * whole struct, so every path that can reach a slot's copy goes through a
     * seed first and the handle is never left NULL. */
    sim->clientKnownMap[slot] = &sim->clientKnownMapObj[slot];
    /* Ahead of the map check below so a slot binds both records together: an
     * unseeded pill record means "fall back to the live list", which is the
     * wrong side to fail on once positions are being withheld. */
    serverSimPillShadowSeed(sim, slot);
    if (sim->sim.mp == NULL) return;
    memcpy(sim->clientKnownMapObj[slot].mapItem, (*sim->sim.mp).mapItem,
           sizeof(sim->clientKnownMapObj[slot].mapItem));
}

void serverSimPillShadowSeed(ServerSim *sim, BYTE slot) {
    BYTE np;
    BYTE p;
    if (sim == NULL || slot >= MAX_TANKS) return;
    /* Marked here rather than after the copy, for the same reason the terrain
     * seed binds its handle first: every path that can reach a slot's record
     * goes through a seed, so the slot is never left looking unseeded. */
    sim->clientKnownPillValid[slot] = true;
    if (sim->sim.pb == NULL) return;
    np = pillsGetNumPills(&sim->sim.pb);
    for (p = 0; p < np && p < MAX_PILLS; p++) {
        sim->clientKnownPillX[slot][p] = (*sim->sim.pb).item[p].x;
        sim->clientKnownPillY[slot][p] = (*sim->sim.pb).item[p].y;
    }
}

void serverSimShadowCaptureRoundStart(ServerSim *sim) {
    if (sim == NULL || sim->sim.mp == NULL) return;
    memcpy(sim->roundStartMapObj.mapItem, (*sim->sim.mp).mapItem,
           sizeof(sim->roundStartMapObj.mapItem));
    /* Bound after the copy, not before it as the per-slot seed does: a NULL
     * handle here has to mean "nothing has been captured", so the seed below
     * can fall back to the live map rather than hand out a zeroed one. */
    sim->roundStartMap = &sim->roundStartMapObj;
    serverSimPillShadowCaptureRoundStart(sim);
}

void serverSimPillShadowCaptureRoundStart(ServerSim *sim) {
    BYTE np;
    BYTE p;
    if (sim == NULL || sim->sim.pb == NULL) return;
    np = pillsGetNumPills(&sim->sim.pb);
    if (np > MAX_PILLS) np = MAX_PILLS;
    for (p = 0; p < np; p++) {
        sim->roundStartPillX[p] = (*sim->sim.pb).item[p].x;
        sim->roundStartPillY[p] = (*sim->sim.pb).item[p].y;
    }
    sim->roundStartPillCount = np;
    /* Flagged after the copy, not before it as the per-slot seed does: false
     * here has to mean "nothing has been captured", so the seed below can fall
     * back to the live list rather than hand out zeroed squares. */
    sim->roundStartPillsValid = true;
}

void serverSimShadowSeedAll(ServerSim *sim) {
    BYTE slot;
    for (slot = 0; slot < MAX_TANKS; slot++) {
        serverSimShadowSeed(sim, slot);
    }
    /* Every caller of SeedAll is a point where a map is installed — sim create,
     * the map loaders, a lobby map change, the round reset — which is exactly
     * where the round-start copy has to be taken, so it is taken here rather
     * than from a second list of call sites that could drift from this one. */
    serverSimShadowCaptureRoundStart(sim);
}

void serverSimShadowSeedRoundStart(ServerSim *sim, BYTE slot) {
    if (sim == NULL || slot >= MAX_TANKS) return;
    if (sim->roundStartMap == NULL) {
        /* No map has been installed since create, so there is no round-start
         * terrain to differ from the live map. */
        serverSimShadowSeed(sim, slot);
        return;
    }
    sim->clientKnownMap[slot] = &sim->clientKnownMapObj[slot];
    memcpy(sim->clientKnownMapObj[slot].mapItem, sim->roundStartMapObj.mapItem,
           sizeof(sim->clientKnownMapObj[slot].mapItem));
    serverSimPillShadowSeedRoundStart(sim, slot);
}

void serverSimPillShadowSeedRoundStart(ServerSim *sim, BYTE slot) {
    BYTE p;
    if (sim == NULL || slot >= MAX_TANKS) return;
    if (!sim->roundStartPillsValid) {
        /* No map has been installed since create, so there are no round-start
         * squares to differ from the live list. */
        serverSimPillShadowSeed(sim, slot);
        return;
    }
    sim->clientKnownPillValid[slot] = true;
    for (p = 0; p < sim->roundStartPillCount && p < MAX_PILLS; p++) {
        sim->clientKnownPillX[slot][p] = sim->roundStartPillX[p];
        sim->clientKnownPillY[slot][p] = sim->roundStartPillY[p];
    }
}

void serverSimShadowApplySlot(ServerSim *sim, BYTE slot, BYTE x, BYTE y,
                              BYTE terrain) {
    if (sim == NULL || slot >= MAX_TANKS) return;
    if (sim->clientKnownMap[slot] == NULL) return;
    sim->clientKnownMapObj[slot].mapItem[x][y] = terrain;
}

void serverSimShadowApply(ServerSim *sim, BYTE x, BYTE y, BYTE terrain) {
    BYTE slot;
    if (sim == NULL) return;
    /* Written per slot rather than once for all of them: each slot's copy
     * records what that client was sent, so which slots take a given change
     * is a per-slot decision. */
    for (slot = 0; slot < MAX_TANKS; slot++) {
        serverSimShadowApplySlot(sim, slot, x, y, terrain);
    }
}

void serverSimSetShadowCulled(ServerSim *sim, BYTE slot, bool culled) {
    if (sim == NULL || slot >= MAX_TANKS) return;
    if (culled) {
        sim->shadowCulledSlots |= (uint16_t)(1u << slot);
    } else {
        sim->shadowCulledSlots &= (uint16_t)~(1u << slot);
    }
}

bool serverSimIsShadowCulled(const ServerSim *sim, BYTE slot) {
    if (sim == NULL || slot >= MAX_TANKS) return false;
    return (sim->shadowCulledSlots & (uint16_t)(1u << slot)) != 0;
}

uint16_t serverSimGetShadowCulledMask(const ServerSim *sim) {
    return (sim == NULL) ? 0u : sim->shadowCulledSlots;
}

void serverSimShadowTick(ServerSim *sim) {
    BYTE slot;
    uint16_t e;

    if (sim == NULL) return;
    for (slot = 0; slot < MAX_TANKS; slot++) {
        if (sim->clientKnownMap[slot] == NULL) continue;
        /* A culled slot's copy advances in the UDP drain instead, one tile per
         * event that slot is actually sent. Advancing it here would claim the
         * client had been told about changes the drain then culls. */
        if (serverSimIsShadowCulled(sim, slot)) continue;
        /* EVENT_MAP_CHANGE data is [mx, my, newTerrain] — the same triple
         * simMapChangeCallback records and the client replays through
         * mapSetPos. */
        for (e = 0; e < sim->mapEventCount; e++) {
            serverSimShadowApplySlot(sim, slot, sim->mapEvents[e].data[0],
                                     sim->mapEvents[e].data[1],
                                     sim->mapEvents[e].data[2]);
        }
    }
    serverSimPillShadowTick(sim);
}

bool serverSimPillPosVisible(ServerSim *sim, BYTE slot, BYTE pillIdx,
                             const ViewportRect *vps, int numVps) {
    if (sim == NULL || slot >= MAX_TANKS || sim->sim.pb == NULL) return false;
    if (pillIdx >= pillsGetNumPills(&sim->sim.pb) || pillIdx >= MAX_PILLS) {
        return false;
    }
    /* Nothing has been recorded for this slot, so its checksum and its blob are
     * taken over the live pill list. Withholding here would leave the square it
     * is sent disagreeing with the square its checksum was taken over. */
    if (!sim->clientKnownPillValid[slot]) return true;
    /* An advantage brain is promised the location of every pillbox on the map
     * even out of visual range, and that promise is served out of the bot's own
     * client data — fogging its snapshot would quietly make the lobby option
     * untrue. A plain computer player gets the same view a human does. */
    if (serverSimIsBot(sim, slot) &&
        (sim->botMgr.bots[slot].ai == aiYesAdvantage ||
         sim->botMgr.bots[slot].ai == aiFull)) {
        return true;
    }
    return inAnyViewport(vps, numVps, (*sim->sim.pb).item[pillIdx].x,
                         (*sim->sim.pb).item[pillIdx].y);
}

void serverSimFogPillUpdateEvent(ServerSim *sim, BYTE slot, GameEvent *ev,
                                 const ViewportRect *vps, int numVps) {
    BYTE p;

    if (sim == NULL || ev == NULL || slot >= MAX_TANKS) return;
    p = ev->data[0];
    if (sim->sim.pb == NULL || p >= pillsGetNumPills(&sim->sim.pb) ||
        p >= MAX_PILLS) {
        return;
    }
    if (serverSimPillPosVisible(sim, slot, p, vps, numVps)) {
        ev->data[4] = pillSetPosCurrent(ev->data[4], true);
        return;
    }
    /* Owner and armourInTank's own bits are left as they are: they are public
     * and have to keep arriving for a pill nobody can see. */
    ev->data[1] = sim->clientKnownPillX[slot][p];
    ev->data[2] = sim->clientKnownPillY[slot][p];
    ev->data[4] = pillSetPosCurrent(ev->data[4], false);
}

void serverSimPillShadowTick(ServerSim *sim) {
    ViewportRect vps[MAX_VIEWPORTS];
    BYTE slot;
    BYTE np;
    BYTE p;

    if (sim == NULL || sim->sim.pb == NULL) return;
    np = pillsGetNumPills(&sim->sim.pb);
    if (np > MAX_PILLS) np = MAX_PILLS;
    /* A slot's record holds the last square that recipient was given, so only
     * the pills it can see take this frame's move; one it cannot see keeps the
     * square it had, however far the pill has since travelled. It runs here, at
     * the end of the tick, so both event filters and every checksum read a
     * record that already holds this frame's moves. */
    for (slot = 0; slot < MAX_TANKS; slot++) {
        int numVps;
        if (!sim->clientKnownPillValid[slot] || !sim->playerConnected[slot]) {
            continue;
        }
        numVps = serverSimBuildViewports(sim, slot, vps, MAX_VIEWPORTS);
        for (p = 0; p < np; p++) {
            if (!serverSimPillPosVisible(sim, slot, p, vps, numVps)) continue;
            sim->clientKnownPillX[slot][p] = (*sim->sim.pb).item[p].x;
            sim->clientKnownPillY[slot][p] = (*sim->sim.pb).item[p].y;
        }
    }
}

bool serverSimGetPillsForSlot(ServerSim *sim, BYTE slot, struct pillsObj *out) {
    BYTE p;
    if (sim == NULL || out == NULL || slot >= MAX_TANKS ||
        sim->sim.pb == NULL || !sim->clientKnownPillValid[slot]) {
        return false;
    }
    /* Only the square comes from the record: owner, armour, inTank and the
     * rest are public and go out as the live list holds them. */
    *out = *sim->sim.pb;
    for (p = 0; p < out->numPills && p < MAX_PILLS; p++) {
        out->item[p].x = sim->clientKnownPillX[slot][p];
        out->item[p].y = sim->clientKnownPillY[slot][p];
    }
    return true;
}

int serverSimGetCompressedMapFor(ServerSim *sim, BYTE slot, BYTE *output,
                                 int outputCap) {
    if (sim == NULL || slot >= MAX_TANKS || sim->clientKnownMap[slot] == NULL) {
        return 0;
    }
    struct pillsObj slotPills;
    pillboxes slotPb = &slotPills;
    bool useSlotPills = serverSimGetPillsForSlot(sim, slot, &slotPills);
    return mapSaveCompressedMap(&sim->clientKnownMap[slot],
                                useSlotPills ? &slotPb : &sim->sim.pb,
                                &sim->sim.bs, &sim->sim.ss, output, outputCap);
}

int serverSimShadowSweep(ServerSim *sim, BYTE slot, const ViewportRect *vps,
                         int numVps, GameEvent *out, int maxOut) {
    int count = 0;
    int r;

    if (sim == NULL || slot >= MAX_TANKS || vps == NULL || out == NULL) return 0;
    if (maxOut <= 0 || sim->clientKnownMap[slot] == NULL || sim->sim.mp == NULL) {
        return 0;
    }

    for (r = 0; r < numVps && count < maxOut; r++) {
        /* The rects are built around map squares without a bounds check, so a
         * view near an edge runs off the map — clamp before indexing. */
        int minX = vps[r].minMX < 0 ? 0 : vps[r].minMX;
        int minY = vps[r].minMY < 0 ? 0 : vps[r].minMY;
        int maxX = vps[r].maxMX > MAP_ARRAY_SIZE - 1 ? MAP_ARRAY_SIZE - 1 : vps[r].maxMX;
        int maxY = vps[r].maxMY > MAP_ARRAY_SIZE - 1 ? MAP_ARRAY_SIZE - 1 : vps[r].maxMY;
        int x;

        if (minX > maxX || minY > maxY) continue;  /* wholly off the map */

        for (x = minX; x <= maxX && count < maxOut; x++) {
            BYTE *known = sim->clientKnownMapObj[slot].mapItem[x];
            const BYTE *live = (*sim->sim.mp).mapItem[x];
            int y;

            /* Nearly every row of a mostly-current copy matches, so compare the
             * whole span first and skip the per-square walk when it does. */
            if (memcmp(known + minY, live + minY,
                       (size_t)(maxY - minY + 1)) == 0) {
                continue;
            }
            for (y = minY; y <= maxY && count < maxOut; y++) {
                if (known[y] == live[y]) continue;
                out[count].type = EVENT_MAP_CHANGE;
                memset(out[count].data, 0, sizeof(out[count].data));
                out[count].data[0] = (BYTE)x;
                out[count].data[1] = (BYTE)y;
                out[count].data[2] = live[y];
                /* The caller queues every event returned, so the copy takes the
                 * square here. It also keeps overlapping rects from emitting
                 * the same square twice — the second pass finds them equal. */
                known[y] = live[y];
                count++;
            }
        }
    }
    return count;
}

/* One screen-sized rect centred on a map square. */
static void addViewRect(ViewportRect *out, int *n, int centerMX, int centerMY,
                        int halfView) {
    out[*n].minMX = centerMX - halfView; out[*n].maxMX = centerMX + halfView;
    out[*n].minMY = centerMY - halfView; out[*n].maxMY = centerMY + halfView;
    (*n)++;
}

/* Chebyshev distance in map squares — the shape of the decay proximity test. */
static bool viewNearSquare(int aMX, int aMY, int bMX, int bMY) {
    int dx = aMX - bMX;
    int dy = aMY - bMY;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= VIEW_DECAY_NEAR_TILES && dy <= VIEW_DECAY_NEAR_TILES;
}

/* Whether a category grants rects by sweeping every item. viewPolicyKey does
 * not — it grants the single item the recipient reports viewing, built by
 * addKeyViewRect below — and viewPolicyOff grants nothing at all. */
static bool viewCategorySweeps(const ServerSim *sim, ViewCategory cat) {
    ViewPolicy policy = sim->viewPolicy[cat];
    return policy == viewPolicyAlways || policy == viewPolicyDecay;
}

/* Whether an item is one this recipient may see through at all, policy aside:
 * an allied pillbox that is alive and on the map rather than in a tank (the
 * same test as pillsCanView, own pills counting as allied), an owned allied
 * base — a neutral base belongs to nobody, so it never grants a view — and an
 * allied tank that exists and is not waiting out a death. Each takes the raw
 * index and reports false when it is out of range, so both the sweeps below
 * and serverSimValidateViewTargets can hand them an unchecked value. */
static bool viewPillQualifies(ServerSim *sim, BYTE clientIdx, BYTE p) {
    if (sim->sim.pb == NULL || p >= pillsGetNumPills(&sim->sim.pb)) return false;
    if (!playersIsAllie(&sim->sim.plyrs, (*sim->sim.pb).item[p].owner,
                        clientIdx)) return false;
    if ((*sim->sim.pb).item[p].armour == 0) return false;
    if ((*sim->sim.pb).item[p].inTank) return false;
    return true;
}

static bool viewBaseQualifies(ServerSim *sim, BYTE clientIdx, BYTE b) {
    BYTE owner;
    if (sim->sim.bs == NULL || b >= basesGetNumBases(&sim->sim.bs)) return false;
    owner = (*sim->sim.bs).item[b].owner;
    if (owner == NEUTRAL) return false;
    return playersIsAllie(&sim->sim.plyrs, owner, clientIdx);
}

static bool viewAllyQualifies(ServerSim *sim, BYTE clientIdx, BYTE t) {
    if (t >= MAX_TANKS || t == clientIdx) return false;
    if (!playersIsAllie(&sim->sim.plyrs, t, clientIdx)) return false;
    if (sim->sim.tanks[t] == NULL) return false;
    return tankGetDeathWait(&sim->sim.tanks[t]) == 0;
}

/* Whether a qualifying item counts for this recipient right now. Under
 * viewPolicyAlways every qualifying item does; under viewPolicyDecay only
 * while its proximity clock is inside the category's window. A clock of 0
 * means the player has never been near the item. */
static bool viewItemInWindow(const ServerSim *sim, ViewCategory cat,
                             uint32_t nearTick) {
    if (sim->viewPolicy[cat] != viewPolicyDecay) {
        return true;
    }
    if (nearTick == 0) {
        return false;
    }
    /* A running frame advances sim->tick by two half-steps, so seconds convert
     * at GAME_NUMTOTALTICKS_SEC. */
    return (sim->tick - nearTick) <=
           (uint32_t)sim->viewDecaySecs[cat] * GAME_NUMTOTALTICKS_SEC;
}

/* The single rule for "this recipient may watch that ally right now": the
 * ally category is not switched off, the ally qualifies, and — under
 * viewPolicyDecay — the recipient's proximity clock for it is still inside
 * the window. serverSimPickAlly and the ALLY arm of
 * serverSimValidateViewTargets both ask this one function, so a pick cannot
 * be dropped by the keep-this-view pass the tick after it is made. */
static bool viewAllyWatchable(ServerSim *sim, BYTE clientIdx, BYTE t) {
    return sim->viewPolicy[viewCategoryAlly] != viewPolicyOff &&
           viewAllyQualifies(sim, clientIdx, t) &&
           viewItemInWindow(sim, viewCategoryAlly,
                            sim->allyNearTick[clientIdx][t]);
}

/* An ally's current map square, taken from its tank. The server holds real
 * positions, so the players table's last-known copy is not needed here. */
static void viewAllySquare(ServerSim *sim, BYTE t, BYTE *mx, BYTE *my) {
    WORLD wx = 0, wy = 0;
    tankGetWorld(&sim->sim.tanks[t], &wx, &wy);
    *mx = (BYTE)(wx >> 8);
    *my = (BYTE)(wy >> 8);
}

/* Step through the watchable allies in slot order, `step` +1 for next and -1
 * for previous. From an origin the scan starts at the neighbouring slot and
 * covers MAX_TANKS candidates, ending back on the origin, so a lone watchable
 * ally is offered again rather than reported missing. With no origin it starts
 * at whichever end the direction comes from. Returns MAX_TANKS when nothing is
 * watchable. */
static BYTE viewStepAlly(ServerSim *sim, BYTE clientIdx, bool haveOrigin,
                         BYTE origin, int step) {
    int i;

    if (!haveOrigin) {
        for (i = 0; i < MAX_TANKS; i++) {
            BYTE t = (BYTE)((step > 0) ? i : (MAX_TANKS - 1 - i));
            if (viewAllyWatchable(sim, clientIdx, t)) return t;
        }
        return MAX_TANKS;
    }
    for (i = 1; i <= MAX_TANKS; i++) {
        int idx = ((int)origin + step * i) % MAX_TANKS;
        if (idx < 0) idx += MAX_TANKS;
        if (viewAllyWatchable(sim, clientIdx, (BYTE)idx)) return (BYTE)idx;
    }
    return MAX_TANKS;
}

/* Nearest watchable ally in one of the four scroll directions, measured from
 * the origin ally's square. One axis at a time — a horizontal press only
 * considers allies strictly left or strictly right, a vertical one only
 * strictly above or below — and the shortest straight-line distance wins.
 * That is what playersMoveAllyView does for the client's own stepping, so the
 * server picks what the client would have picked. The inequalities are strict,
 * so an ally sharing the origin's square never matches. Returns MAX_TANKS when
 * nothing lies that way. */
static BYTE viewNearestAllyInDirection(ServerSim *sim, BYTE clientIdx,
                                       BYTE origin, uint8_t direction) {
    BYTE originX = 0, originY = 0;
    BYTE found = MAX_TANKS;
    double nearest = 65000;
    BYTE t;

    viewAllySquare(sim, origin, &originX, &originY);

    for (t = 0; t < MAX_TANKS; t++) {
        BYTE itemX = 0, itemY = 0;
        bool matches = false;
        double dist;

        if (t == origin) continue;
        if (!viewAllyWatchable(sim, clientIdx, t)) continue;
        viewAllySquare(sim, t, &itemX, &itemY);

        switch (direction) {
        case VIEW_CYCLE_LEFT:  matches = (itemX < originX); break;
        case VIEW_CYCLE_RIGHT: matches = (itemX > originX); break;
        case VIEW_CYCLE_UP:    matches = (itemY < originY); break;
        case VIEW_CYCLE_DOWN:  matches = (itemY > originY); break;
        default: break;
        }
        if (!matches) continue;

        if (utilIsItemInRange(originX, originY, itemX, itemY,
                              (WORLD)nearest, &dist)) {
            nearest = dist;
            found = t;
        }
    }
    return found;
}

bool serverSimPickAlly(ServerSim *sim, BYTE clientIdx, uint8_t direction,
                       uint8_t from, BYTE *outTarget, BYTE *outMapX,
                       BYTE *outMapY) {
    bool haveOrigin;
    BYTE origin = 0;
    BYTE found;

    if (clientIdx >= MAX_TANKS) return false;
    if (sim->sim.plyrs == NULL) return false;

    /* `from` is a starting point only while it is still something this
     * recipient may watch; otherwise the request starts from the beginning. */
    haveOrigin = (from < MAX_TANKS && viewAllyWatchable(sim, clientIdx, from));
    if (haveOrigin) {
        origin = from;
    }

    if (haveOrigin && (direction == VIEW_CYCLE_LEFT ||
                       direction == VIEW_CYCLE_RIGHT ||
                       direction == VIEW_CYCLE_UP ||
                       direction == VIEW_CYCLE_DOWN)) {
        found = viewNearestAllyInDirection(sim, clientIdx, origin, direction);
    } else if (direction == VIEW_CYCLE_PREV) {
        found = viewStepAlly(sim, clientIdx, haveOrigin, origin, -1);
    } else {
        /* VIEW_CYCLE_NEXT, a scroll direction with no origin to measure from,
         * and any direction byte off the wire this server does not know. */
        found = viewStepAlly(sim, clientIdx, haveOrigin, origin, 1);
    }

    if (found >= MAX_TANKS) return false;

    *outTarget = found;
    viewAllySquare(sim, found, outMapX, outMapY);
    return true;
}

/* Whether a viewPolicyKey category is granting this recipient a rect right
 * now: it has reported watching an item of that kind (CMD_VIEW_STATE) and the
 * item still qualifies. Under key the player watches one thing at a time, so
 * this is what earns that one item its rect; the recipient's own tank screen
 * is sent alongside it either way. */
static bool viewKeyRectGranted(ServerSim *sim, BYTE clientIdx) {
    BYTE target = sim->viewTarget[clientIdx];

    switch (sim->viewKind[clientIdx]) {
    case VIEW_KIND_PILL:
        return sim->viewPolicy[viewCategoryPill] == viewPolicyKey &&
               viewPillQualifies(sim, clientIdx, target);
    case VIEW_KIND_BASE:
        return sim->viewPolicy[viewCategoryBase] == viewPolicyKey &&
               viewBaseQualifies(sim, clientIdx, target);
    case VIEW_KIND_ALLY:
        return sim->viewPolicy[viewCategoryAlly] == viewPolicyKey &&
               viewAllyQualifies(sim, clientIdx, target);
    default:
        return false;  /* tank view, and a kind this server does not know */
    }
}

/* The single rect a viewPolicyKey category grants: the item this recipient
 * last reported viewing through (CMD_VIEW_STATE), while it is still in range
 * and still qualifies. Adds nothing for a tank-view claim, for a category that
 * is not on viewPolicyKey, or once the target stops qualifying — the client is
 * told nothing, the rect just stops appearing. */
static void addKeyViewRect(ServerSim *sim, BYTE clientIdx, ViewportRect *out,
                           int *n, int maxOut, int halfView) {
    BYTE target = sim->viewTarget[clientIdx];

    if (*n >= maxOut) return;
    if (!viewKeyRectGranted(sim, clientIdx)) return;

    switch (sim->viewKind[clientIdx]) {
    case VIEW_KIND_PILL:
        addViewRect(out, n, (*sim->sim.pb).item[target].x,
                    (*sim->sim.pb).item[target].y, halfView);
        return;
    case VIEW_KIND_BASE:
        addViewRect(out, n, (*sim->sim.bs).item[target].x,
                    (*sim->sim.bs).item[target].y, halfView);
        return;
    case VIEW_KIND_ALLY: {
        WORLD wx = 0, wy = 0;
        tankGetWorld(&sim->sim.tanks[target], &wx, &wy);
        addViewRect(out, n, wx >> 8, wy >> 8, halfView);
        return;
    }
    default:
        return;
    }
}

int serverSimBuildViewports(ServerSim *sim, BYTE clientIdx, ViewportRect *out, int maxOut) {
    int n = 0;
    int halfView = (SNAPSHOT_SCREEN_SIZE / 2) + SNAPSHOT_VIEWPORT_MARGIN;
    WORLD clientWX = 0, clientWY = 0;
    bool hasTankRect = false;

    /* The per-recipient view state below is indexed by slot. */
    if (clientIdx >= MAX_TANKS) {
        return 0;
    }

    /* The recipient's own tank screen is always in the set, whatever they are
     * watching. Watching an item under viewPolicyKey adds that item's screen
     * (addKeyViewRect below) rather than trading the tank's away: the client
     * predicts its own tank against the ground round it, so withholding that
     * ground leaves it driving through pills and terrain the server has
     * stopped telling it about and blocking the tank where the client has
     * already moved it. Leaving the tank behind while the player is in an item
     * view is what the screen draws, not what the server sends. */
    if (n < maxOut && serverSimGetTankState(sim, clientIdx, &clientWX, &clientWY)) {
        int centerMX = clientWX >> 8;
        int centerMY = clientWY >> 8;
        /* The square the frozen rect below falls back on once the tank has
         * gone. */
        sim->lastTankMX[clientIdx] = (uint8_t)centerMX;
        sim->lastTankMY[clientIdx] = (uint8_t)centerMY;
        sim->lastTankValid[clientIdx] = true;
        addViewRect(out, &n, centerMX, centerMY, halfView);
        hasTankRect = true;
    }

    /* Allied pillboxes. */
    if (sim->sim.pb != NULL && viewCategorySweeps(sim, viewCategoryPill)) {
        BYTE np = pillsGetNumPills(&sim->sim.pb);
        BYTE p;
        for (p = 0; p < np && n < maxOut; p++) {
            if (!viewPillQualifies(sim, clientIdx, p)) continue;
            if (!viewItemInWindow(sim, viewCategoryPill,
                                  sim->pillNearTick[clientIdx][p])) continue;
            addViewRect(out, &n, (*sim->sim.pb).item[p].x,
                        (*sim->sim.pb).item[p].y, halfView);
        }
    }

    /* Allied bases. */
    if (sim->sim.bs != NULL && viewCategorySweeps(sim, viewCategoryBase)) {
        BYTE nb = basesGetNumBases(&sim->sim.bs);
        BYTE b;
        for (b = 0; b < nb && n < maxOut; b++) {
            if (!viewBaseQualifies(sim, clientIdx, b)) continue;
            if (!viewItemInWindow(sim, viewCategoryBase,
                                  sim->baseNearTick[clientIdx][b])) continue;
            addViewRect(out, &n, (*sim->sim.bs).item[b].x,
                        (*sim->sim.bs).item[b].y, halfView);
        }
    }

    /* Allied tanks. */
    if (viewCategorySweeps(sim, viewCategoryAlly)) {
        BYTE t;
        for (t = 0; t < MAX_TANKS && n < maxOut; t++) {
            WORLD wx = 0, wy = 0;
            if (!viewAllyQualifies(sim, clientIdx, t)) continue;
            if (!viewItemInWindow(sim, viewCategoryAlly,
                                  sim->allyNearTick[clientIdx][t])) continue;
            tankGetWorld(&sim->sim.tanks[t], &wx, &wy);
            addViewRect(out, &n, wx >> 8, wy >> 8, halfView);
        }
    }

    /* The one item this recipient says it is looking through, when its
     * category is on viewPolicyKey. */
    addKeyViewRect(sim, clientIdx, out, &n, maxOut, halfView);

    /* No tank this build: hold the view at the square the tank was last seen
     * at. A slot that never had a tank gets whatever the policies produced,
     * which can be nothing. */
    if (!hasTankRect && n < maxOut && sim->lastTankValid[clientIdx]) {
        addViewRect(out, &n, sim->lastTankMX[clientIdx],
                    sim->lastTankMY[clientIdx], halfView);
    }

    return n;
}

void serverSimUpdateViewDecay(ServerSim *sim) {
    /* 0 is the "never been near" marker, so tick 0 stamps as 1. A stamp that
     * lands on tick 0 therefore reads as not-viewable for that one frame —
     * viewItemInWindow takes tick - nearTick, which underflows to a huge age —
     * and comes good on the next one. Tick 0 is the frame the sim starts on,
     * with nobody connected to notice. */
    uint32_t stamp = (sim->tick == 0) ? 1u : sim->tick;
    bool pillDecay = (sim->viewPolicy[viewCategoryPill] == viewPolicyDecay);
    bool baseDecay = (sim->viewPolicy[viewCategoryBase] == viewPolicyDecay);
    bool allyDecay = (sim->viewPolicy[viewCategoryAlly] == viewPolicyDecay);
    BYTE c;

    if (!pillDecay && !baseDecay && !allyDecay) {
        return;
    }

    for (c = 0; c < MAX_TANKS; c++) {
        WORLD wx = 0, wy = 0;
        int mx, my;

        if (!sim->playerConnected[c]) continue;
        if (sim->sim.tanks[c] == NULL) continue;
        if (tankGetDeathWait(&sim->sim.tanks[c]) != 0) continue;
        tankGetWorld(&sim->sim.tanks[c], &wx, &wy);
        mx = wx >> 8;
        my = wy >> 8;

        /* Every item is stamped, whoever owns it — alliance, armour and the
         * rest are the build's business, and an item can change hands long
         * after the player drove past it. */
        if (pillDecay && sim->sim.pb != NULL) {
            BYTE np = pillsGetNumPills(&sim->sim.pb);
            BYTE p;
            for (p = 0; p < np; p++) {
                if (viewNearSquare(mx, my, (*sim->sim.pb).item[p].x,
                                   (*sim->sim.pb).item[p].y)) {
                    sim->pillNearTick[c][p] = stamp;
                }
            }
        }
        if (baseDecay && sim->sim.bs != NULL) {
            BYTE nb = basesGetNumBases(&sim->sim.bs);
            BYTE b;
            for (b = 0; b < nb; b++) {
                if (viewNearSquare(mx, my, (*sim->sim.bs).item[b].x,
                                   (*sim->sim.bs).item[b].y)) {
                    sim->baseNearTick[c][b] = stamp;
                }
            }
        }
        if (allyDecay) {
            BYTE t;
            /* A dead ally is stamped like any other item. Its tank still
             * carries a position — tankDeath moves it to its restart square at
             * once — and whether it may be seen through is decided at build
             * time by viewAllyQualifies, which does test deathWait. The
             * deathWait skip above is for the player doing the looking, not
             * for the item being looked at, so the two are not symmetric. */
            for (t = 0; t < MAX_TANKS; t++) {
                WORLD twx = 0, twy = 0;
                if (t == c) continue;
                if (sim->sim.tanks[t] == NULL) continue;
                tankGetWorld(&sim->sim.tanks[t], &twx, &twy);
                if (viewNearSquare(mx, my, twx >> 8, twy >> 8)) {
                    sim->allyNearTick[c][t] = stamp;
                }
            }
        }
    }
}

void serverSimValidateViewTargets(ServerSim *sim) {
    BYTE c;

    for (c = 0; c < MAX_TANKS; c++) {
        BYTE target = sim->viewTarget[c];
        bool keep = false;

        if (!sim->playerConnected[c]) continue;
        if (sim->viewKind[c] == VIEW_KIND_TANK) continue;

        /* The window test is what viewPolicyDecay adds: it passes for every
         * other policy, so under key there is nothing extra to check. */
        switch (sim->viewKind[c]) {
        case VIEW_KIND_PILL:
            keep = sim->viewPolicy[viewCategoryPill] != viewPolicyOff &&
                   viewPillQualifies(sim, c, target) &&
                   viewItemInWindow(sim, viewCategoryPill,
                                    sim->pillNearTick[c][target]);
            break;
        case VIEW_KIND_BASE:
            keep = sim->viewPolicy[viewCategoryBase] != viewPolicyOff &&
                   viewBaseQualifies(sim, c, target) &&
                   viewItemInWindow(sim, viewCategoryBase,
                                    sim->baseNearTick[c][target]);
            break;
        case VIEW_KIND_ALLY:
            keep = viewAllyWatchable(sim, c, target);
            break;
        default:
            break;  /* a kind this server does not know */
        }

        if (!keep) {
            sim->viewKind[c]   = VIEW_KIND_TANK;
            sim->viewTarget[c] = 0;
        }
    }
}

/* The exemptions the tank hide and the man hide share: nothing at (tx,ty) is
 * hidden from a recipient inside MIN_TREEHIDE_DIST on both axes, and an ally is
 * exempt while the server's allies-in-trees option is on. A recipient with no
 * tank of its own gets no tree hide at all. */
static bool treeHideExempt(ServerSim *sim, BYTE viewer, BYTE target,
                           WORLD tx, WORLD ty) {
    WORLD vx = 0, vy = 0;
    int dx, dy;

    if (!serverSimGetTankState(sim, viewer, &vx, &vy)) return true;

    dx = (int)tx - (int)vx;
    if (dx < 0) dx = -dx;
    dy = (int)ty - (int)vy;
    if (dy < 0) dy = -dy;
    if (dx < MIN_TREEHIDE_DIST && dy < MIN_TREEHIDE_DIST) return true;

    return serverSimGetAlliesInTrees(sim) &&
           playersIsAllie(&sim->sim.plyrs, target, viewer);
}

/* A tank standing in trees is withheld from a recipient more than
 * MIN_TREEHIDE_DIST away on either axis. Firing gives the position away — the
 * shot clears the trees the tank was hiding in. */
static bool tankHiddenInTrees(ServerSim *sim, BYTE viewer, BYTE target,
                              WORLD tx, WORLD ty) {
    if (treeHideExempt(sim, viewer, target, tx, ty)) return false;
    if (tankJustFired(&sim->sim.tanks[target])) return false;

    return utilIsTankInTrees(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, tx, ty);
}

/* Whether a target's man is something this recipient may see. He is his own
 * thing to hide, not part of the tank that sent him out: the parachute is
 * always shown, a man standing anywhere but trees is always shown, and the
 * tree rule that can hide him is measured on his own square and his own
 * distance from the recipient. False when there is no man out to send. */
static bool lgmVisibleToViewer(ServerSim *sim, BYTE viewer, BYTE target) {
    WORLD lx, ly;

    if (sim->sim.lgmen[target] == NULL) return false;
    if (!lgmIsOut(&sim->sim.lgmen[target])) return false;
    if (lgmGetFrame(&sim->sim.lgmen[target]) == LGM_HELICOPTER_FRAME) return true;

    lx = (WORLD)((lgmGetMX(&sim->sim.lgmen[target]) << TANK_SHIFT_MAPSIZE) +
                 (lgmGetPX(&sim->sim.lgmen[target]) << TANK_SHIFT_RIGHT2));
    ly = (WORLD)((lgmGetMY(&sim->sim.lgmen[target]) << TANK_SHIFT_MAPSIZE) +
                 (lgmGetPY(&sim->sim.lgmen[target]) << TANK_SHIFT_RIGHT2));

    if (treeHideExempt(sim, viewer, target, lx, ly)) return true;

    return !utilIsTankInTrees(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, lx, ly);
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
     * whatever the view policies allow; under noCull, a single full-map
     * viewport so everything is sent. */
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
        bool tankWithheld = false;

        if (!sim->playerConnected[i]) continue;
        if (!serverSimGetTankState(sim, (BYTE)i, &wx, &wy)) continue;

        /* Always include the client's own tank; cull others by viewport.
         * A tank and its man are culled apart — either one being visible is
         * enough to send the entry — and the entry then carries only the half
         * the recipient may see.  A slot with neither visible is emitted as a
         * 1-byte stub (TANK_SNAPSHOT_HIDDEN_FLAG) rather than skipped, so the
         * client can clear stale ghost positions for tanks that have driven
         * off screen.  noCull bypasses all of this so recording paths capture
         * every tank in full. */
        if (i != clientIdx && !noCull) {
            bool tankInView = inAnyViewport(viewports, numViewports, wx >> 8, wy >> 8);
            bool manInView = false;

            /* The man is culled on his own square, not his tank's: he can be a
             * long way from it — parachuting in from a spawn, or off building
             * — and whether the recipient may see him is his own question
             * (lgmVisibleToViewer), decided before his square is tested
             * against the rects. */
            if (lgmVisibleToViewer(sim, clientIdx, (BYTE)i)) {
                BYTE lgmMX = lgmGetMX(&sim->sim.lgmen[i]);
                BYTE lgmMY = lgmGetMY(&sim->sim.lgmen[i]);
                if (lgmMX != 0 || lgmMY != 0) {
                    manInView = inAnyViewport(viewports, numViewports, lgmMX, lgmMY);
                }
            }

            /* Tree hide takes the tank away, not the man standing outside it:
             * with the man still on screen the entry goes out carrying him,
             * with the tank's own fields withheld below. */
            if (tankInView && tankHiddenInTrees(sim, clientIdx, (BYTE)i, wx, wy)) {
                tankInView = false;
                tankWithheld = true;
            }

            if (!tankInView && !manInView) {
                ts = &tanksOut[tankCount];
                memset(ts, 0, sizeof(*ts));
                ts->playerNum = (uint8_t)(i | TANK_SNAPSHOT_HIDDEN_FLAG);
                tankCount++;
                continue;
            }
        }

        ts = &tanksOut[tankCount];
        ts->playerNum = (uint8_t)i;
        ts->hiddenFlags = 0;
        ts->worldX = wx;
        ts->worldY = wy;
        ts->angle = (uint16_t)(tankGetAngle(&sim->sim.tanks[i]) * 256.0f);
        ts->speed = (uint16_t)(tankGetActualSpeed(&sim->sim.tanks[i]) * 256.0f);
        {
            /* Both death signals go to every recipient: the wait, which the
             * interpolation reads, and the destroyed state, which is the only
             * way a non-owner learns a tank is destroyed once its wait has
             * run out but no start has been found. */
            uint8_t status = 0;
            if (tankIsOnBoat(&sim->sim.tanks[i])) status |= TANK_STATUS_ON_BOAT;
            if (tankGetDeathWait(&sim->sim.tanks[i]) > 0) status |= TANK_STATUS_DEAD;
            if (tankIsDestroyed(&sim->sim.tanks[i])) status |= TANK_STATUS_DESTROYED;
            ts->tankStatus = status;
        }
        ts->lgmFrame = lgmIsOut(&sim->sim.lgmen[i]) ? (lgmGetFrame(&sim->sim.lgmen[i]) + 1) : 0;
        ts->lgmMX = lgmGetMX(&sim->sim.lgmen[i]);
        ts->lgmMY = lgmGetMY(&sim->sim.lgmen[i]);
        ts->lgmPX = lgmGetPX(&sim->sim.lgmen[i]);
        ts->lgmPY = lgmGetPY(&sim->sim.lgmen[i]);
        ts->firstLeft = tankGetFirstLeft(&sim->sim.tanks[i]);
        ts->firstRight = tankGetFirstRight(&sim->sim.tanks[i]);
        ts->pingMs = sim->playerPing[i];
        {
            uint8_t cf = playersGetClientFlags(&sim->sim.plyrs, (BYTE)i);
            /* The mic bits are shown only to the players this one's voice
             * could reach: everyone outside a running game (all-talk), the
             * live alliance inside one. Same scope rule serverPumpVoice
             * carries the frames themselves by, so a mic status can never
             * appear for someone you cannot hear. Safe because a snapshot
             * is built per recipient — never cache or share the result.
             * Every other bit stays recipient-agnostic. */
            if (i != clientIdx &&
                serverSimGetState(sim) == serverStateRunning &&
                !playersIsAllie(&sim->sim.plyrs, (BYTE)i, clientIdx)) {
                cf &= (uint8_t)~PLAYER_VOICE_FLAG_MASK;
            }
            ts->clientFlags = cf;
        }
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
            {
                /* The owning client predicts with these, so they ride the
                 * same owner-only path the resources do. All zero on an
                 * unmodified tank, which keeps the group off the wire. */
                TankModifiers mods;
                tankGetModifiers(sim->sim.tanks[i], &mods);
                ts->modSpeed = mods.speed;
                ts->modAccel = mods.accel;
                ts->modTurn = mods.turn;
                ts->modReload = mods.reload;
                ts->modDealt = mods.dealt;
                ts->modTaken = mods.taken;
            }
        } else {
            ts->armour = 0;
            ts->shells = 0;
            ts->mines = 0;
            ts->trees = 0;
            ts->gunsightLen = 0;
            ts->deathWait = 0;
            ts->reload = 0;
            ts->modSpeed = 0;
            ts->modAccel = 0;
            ts->modTurn = 0;
            ts->modReload = 0;
            ts->modDealt = 0;
            ts->modTaken = 0;
        }

        /* Tree-hidden tank, man still on screen: the entry is here for the man
         * alone, so the tank's own fields never reach the wire. A client that
         * ignores the flag reads (0,0) rather than the hiding place, and one
         * that honours it leaves the tank out of interpolation entirely. */
        if (tankWithheld) {
            ts->worldX = 0;
            ts->worldY = 0;
            ts->angle = 0;
            ts->speed = 0;
            ts->tankStatus = 0;
            ts->hiddenFlags = TANK_HIDDEN_POSITION;
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
             *    but its true armour once dead/capturable, so the capturable flip shows,
             *    and once the recipient's tank is inside BASE_PREDICT_REVEAL_RANGE so
             *    their client can predict the square becoming drivable rather than
             *    learn it a round trip late. basesArmourVisibleToPlayer holds all
             *    three cases and is shared with the event cull in
             *    transport_udp_server.c so the two cannot drift.
             *    Mirrors the brain fog-of-war in basesGetBrainBaseInRect.
             *  - shells/mines are the private ammo reserve: real for every
             *    neutral/allied base, zeroed for enemy bases. Always sending a
             *    friendly base's stock means the client has it cached before its
             *    display ever switches to that base, so no stale 0/0 flash. */
            for (i = 0; i < hdr->baseCount; i++) {
                BYTE owner = basesOut[i].owner;
                bool friendly = (owner == NEUTRAL) || (owner == clientIdx) ||
                                playersIsAllie(&sim->sim.plyrs, owner, clientIdx);
                if (!basesArmourVisibleToPlayer(&sim->sim, (BYTE)i, clientIdx)) {
                    basesOut[i].armour = BASE_FULL_ARMOUR;
                }
                if (!friendly) {
                    basesOut[i].shells = 0;
                    basesOut[i].mines  = 0;
                }
            }
        }
        hdr->pillCount = (uint8_t)serverSimGetPills(sim, pillsOut, maxPills);
        /* Per-recipient pill visibility (everything but the square is public):
         * a pill inside one of this recipient's rects reports its real square
         * with the position-current bit set; one it cannot see reports the
         * square it was last given, bit clear, while owner, armour and the
         * in-tank flag stay real and keep updating. */
        for (i = 0; i < hdr->pillCount && i < MAX_PILLS; i++) {
            if (serverSimPillPosVisible(sim, clientIdx, (BYTE)i, viewports,
                                        numViewports)) {
                pillsOut[i].armourInTank =
                    pillSetPosCurrent(pillsOut[i].armourInTank, true);
            } else {
                pillsOut[i].x = sim->clientKnownPillX[clientIdx][i];
                pillsOut[i].y = sim->clientKnownPillY[clientIdx][i];
            }
        }
        /* Checksum the terrain and the pill squares this recipient has actually
         * been given — its own records — so the comparison the client makes is
         * against the map and the pill list it was sent. mapCalcChecksum folds
         * the tile under every pill, so the pill list is as much an input to
         * the hash as the terrain is. The recording paths (noCull) have no
         * client records behind them and checksum the live map and pills. */
        {
            struct pillsObj slotPills;
            pillboxes slotPb = &slotPills;
            bool useSlotMap = (!noCull && sim->clientKnownMap[clientIdx] != NULL);
            bool useSlotPills = useSlotMap &&
                                serverSimGetPillsForSlot(sim, clientIdx, &slotPills);
            hdr->mapChecksum = mapCalcChecksum(
                useSlotMap ? &sim->clientKnownMap[clientIdx] : &sim->sim.mp,
                &sim->sim.bs,
                useSlotPills ? &slotPb : &sim->sim.pb);
        }
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

        /* A human recipient is sent a tier and a bearing in place of each
         * sound's map square. A recipient that reads the square keeps it — a
         * bot's observation builder turns it into a relative position vector,
         * and so does the gym agent's — and so does the recording path, which
         * has no client to withhold anything from. Which sounds reach the
         * recipient at all is decided inside soundPickOffer, shared with the
         * UDP drain. */
        bool keepSquare = noCull ||
                          serverSimRecipientKeepsSoundSquares(sim, clientIdx);
        SoundPick pick;
        int s;
        soundPickInit(&pick);

        if (hasClientPos) {
            for (i = 0; i < sim->eventCount; i++) {
                soundPickOffer(&pick, &sim->events[i], clientIdx,
                               clientMX, clientMY, keepSquare);
            }
        }

        /* Copy map events first (from dedicated buffer), then non-sound
         * events, then deduplicated sound events */
        for (i = 0; i < sim->mapEventCount && outCount < maxEvents; i++) {
            eventsOut[outCount++] = sim->mapEvents[i];
        }
        for (i = 0; i < sim->eventCount && outCount < maxEvents; i++) {
            uint8_t evType = sim->events[i].type;
            if (!soundEventIsSound(evType)) {
                /* Filter EVENT_MINE_VISIBLE: tank mines (bit 7 set) go to all,
                 * LGM mines go only to the placer and their allies */
                if (evType == EVENT_MINE_VISIBLE) {
                    BYTE sp = sim->events[i].data[2];
                    if (!(sp & 0x80) && clientIdx != (sp & 0x7F) &&
                        !playersIsAllie(&sim->sim.plyrs, clientIdx, sp)) {
                        continue;
                    }
                }
                /* A ping is a team signal: the sender, its team and its
                 * allies get it, nobody else. */
                if (evType == EVENT_PING) {
                    if (!serverSimPingReachesClient(sim, (BYTE)clientIdx,
                                                    sim->events[i].data[0])) {
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
                /* A pill this recipient cannot see keeps the square it was last
                 * given. Rewritten rather than dropped: this event is the only
                 * carrier for that pill's armour, owner and in-tank flag
                 * between full syncs. Nothing is written into the record here
                 * or in the UDP drain's copy of this filter — the tick wrote
                 * it, before either of them and before the checksum above. */
                if (evType == EVENT_PILL_UPDATE) {
                    GameEvent pillEv = sim->events[i];
                    serverSimFogPillUpdateEvent(sim, clientIdx, &pillEv,
                                                viewports, numViewports);
                    eventsOut[outCount++] = pillEv;
                    continue;
                }
                eventsOut[outCount++] = sim->events[i];
            }
        }
        for (s = 0; s < SOUND_PICK_TYPES && outCount < maxEvents; s++) {
            if (pick.has[s]) {
                eventsOut[outCount++] = pick.ev[s];
            }
        }

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
