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
    if (sim->sim.mp == NULL) return;
    memcpy(sim->clientKnownMapObj[slot].mapItem, (*sim->sim.mp).mapItem,
           sizeof(sim->clientKnownMapObj[slot].mapItem));
}

void serverSimShadowCaptureRoundStart(ServerSim *sim) {
    if (sim == NULL || sim->sim.mp == NULL) return;
    memcpy(sim->roundStartMapObj.mapItem, (*sim->sim.mp).mapItem,
           sizeof(sim->roundStartMapObj.mapItem));
    /* Bound after the copy, not before it as the per-slot seed does: a NULL
     * handle here has to mean "nothing has been captured", so the seed below
     * can fall back to the live map rather than hand out a zeroed one. */
    sim->roundStartMap = &sim->roundStartMapObj;
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
}

int serverSimGetCompressedMapFor(ServerSim *sim, BYTE slot, BYTE *output,
                                 int outputCap) {
    if (sim == NULL || slot >= MAX_TANKS || sim->clientKnownMap[slot] == NULL) {
        return 0;
    }
    return mapSaveCompressedMap(&sim->clientKnownMap[slot], &sim->sim.pb,
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

/* Whether a viewPolicyKey category is granting this recipient a rect right
 * now: it has reported watching an item of that kind (CMD_VIEW_STATE) and the
 * item still qualifies. Under key the player watches one thing at a time, so
 * this is both what earns the item its rect and what takes their own tank
 * screen away for as long as it lasts. */
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
    bool keyView;
    bool hasTankRect = false;

    /* The per-recipient view state below is indexed by slot. */
    if (clientIdx >= MAX_TANKS) {
        return 0;
    }

    /* Watching an item under viewPolicyKey replaces the recipient's own tank
     * screen rather than adding to it: the ground round the tank stops being
     * sent for as long as they are looking elsewhere, the way the classic
     * screen leaves the tank behind while the player is in an item view. Their
     * own tank is still sent in full — serverSimBuildSnapshot exempts it from
     * the rects — so it is only what is around it that stops arriving. */
    keyView = viewKeyRectGranted(sim, clientIdx);

    if (n < maxOut && serverSimGetTankState(sim, clientIdx, &clientWX, &clientWY)) {
        int centerMX = clientWX >> 8;
        int centerMY = clientWY >> 8;
        /* Recorded whether or not the rect is built — it is the square the
         * frozen rect below falls back on once the tank has gone. */
        sim->lastTankMX[clientIdx] = (uint8_t)centerMX;
        sim->lastTankMY[clientIdx] = (uint8_t)centerMY;
        sim->lastTankValid[clientIdx] = true;
        if (!keyView) {
            addViewRect(out, &n, centerMX, centerMY, halfView);
            hasTankRect = true;
        }
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
     * at, unless a key view is what took the tank rect away — that one is
     * meant to be gone. A slot that never had a tank gets whatever the
     * policies produced, which can be nothing. */
    if (!hasTankRect && !keyView && n < maxOut && sim->lastTankValid[clientIdx]) {
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
            keep = sim->viewPolicy[viewCategoryAlly] != viewPolicyOff &&
                   viewAllyQualifies(sim, c, target) &&
                   viewItemInWindow(sim, viewCategoryAlly,
                                    sim->allyNearTick[c][target]);
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
        /* Checksum the terrain this recipient has actually been given — its
         * own copy — so the comparison the client makes is against the map it
         * was sent. The recording paths (noCull) have no client copy behind
         * them and checksum the live map. */
        hdr->mapChecksum = (noCull || sim->clientKnownMap[clientIdx] == NULL)
            ? mapCalcChecksum(&sim->sim.mp, &sim->sim.bs, &sim->sim.pb)
            : mapCalcChecksum(&sim->clientKnownMap[clientIdx], &sim->sim.bs,
                              &sim->sim.pb);
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
