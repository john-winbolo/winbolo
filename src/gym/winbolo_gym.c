/*
 * Copyright (c) 1998-2008 John Morrison.
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
 *Name:          WinBolo Gym
 *Filename:      winbolo_gym.c
 *Purpose:
 *  Shared library implementation for ML training.
 *  Each WinBoloGym wraps a ServerSim + ClientSim + local
 *  transport in a self-contained, steppable game instance.
 *
 *  Thread safety: multiple instances are supported but
 *  must not be stepped concurrently — the engine uses a
 *  global activeSim pointer during ticks. Use one instance
 *  per thread, or serialise calls with a mutex.
 *********************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "winbolo_gym.h"

#include "../bolo/global.h"
#include "../bolo/screen.h"
#include "../bolo/client_sim.h"
#include "../bolo/frontend.h"
#include "../bolo/players.h"
#include "../bolo/brain.h"
#include "../bolo/pillbox.h"
#include "../bolo/bases.h"
#include "../bolo/transport.h"
#include "../bolo/input_packet.h"
#include "../bolo/gui_message.h"
#include "../bolo/gametype.h"
#include "../bolo/shells.h"
#include "../bolo/explosions.h"
#include "../bolo/lgm.h"
#include "../bolo/tank.h"
#include "../bolo/util.h"
#include "../server/server_sim.h"

/* Required by the engine — stub for library mode */
bool isInMenu = FALSE;

/* Per-instance game state */
struct WinBoloGym {
    ServerSim   serverSim;
    ClientSim   clientSim;
    Transport   transport;

    BYTE       *cachedMap;
    int         cachedMapLen;

    gameType    gameMode;

    uint32_t    simTickCounter;
    int         gameTickCount;
    bool        needMapInit;
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void gymMessageHandler(const char *message, const char *title) {
    (void)message; (void)title;
}

static void gymSyncSnapshot(WinBoloGym *g) {
    SnapshotHeader snapHdr;
    TankSnapshot snapTanks[MAX_TANKS];
    ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
    ExplosionSnapshot snapExplosions[MAX_SNAPSHOT_EXPLOSIONS];
    BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
    PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
    GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];

    if (g->transport.getSnapshot(g->transport.ctx, 0,
                                  &snapHdr, snapTanks, MAX_TANKS,
                                  snapShells, MAX_SNAPSHOT_SHELLS,
                                  snapExplosions, MAX_SNAPSHOT_EXPLOSIONS,
                                  snapBases, MAX_SNAPSHOT_BASES,
                                  snapPills, MAX_SNAPSHOT_PILLS,
                                  snapEvents, MAX_SNAPSHOT_EVENTS)) {
        clientSimSyncFromSnapshot(&g->clientSim, &snapHdr,
                                  snapTanks, snapHdr.tankCount,
                                  snapShells, snapHdr.shellCount,
                                  snapExplosions, snapHdr.explosionCount,
                                  snapBases, snapHdr.baseCount,
                                  snapPills, snapHdr.pillCount,
                                  snapEvents, snapHdr.reliableEventCount, 0);
    }
}

static void gymSetupGame(WinBoloGym *g) {
    serverSimResetGameWorld(&g->serverSim);
    g->serverSim.lobbyEnabled = false;
    g->serverSim.state = serverStateRunning;
    serverSimAddPlayer(&g->serverSim, 0, "GymAgent");
    (*g->serverSim.sim.plyrs).myPlayerNum = 0;

    g->transport = transportLocalCreate(&g->serverSim, 0);

    screenLoadCompressedMapCS(&g->clientSim, g->cachedMap, g->cachedMapLen,
                              "Gym", g->gameMode, false, 0,
                              UNLIMITED_GAME_TIME, "GymAgent", 0, FALSE);
    screenSetAiTypeCS(&g->clientSim, aiYes);
    gymSyncSnapshot(g);
    screenNetSetupTankGoCS(&g->clientSim);

    g->simTickCounter = 0;
    g->gameTickCount = 0;
    g->needMapInit = TRUE;
}

static void gymTeardownGame(WinBoloGym *g) {
    clientSimDestroy(&g->clientSim);
    transportLocalDestroy(&g->transport);
}

/* Check win condition: all bases owned by the same alliance, all with
 * armour above capture threshold.  Mirrors serverSimCheckGameWin(). */
static bool gymCheckGameWin(WinBoloGym *g, bool *agentWon) {
    BYTE max = basesGetNumBases(&g->serverSim.sim.bs);
    if (max == 0) return false;

    BYTE first = NEUTRAL;
    for (BYTE i = 1; i <= max; i++) {
        BYTE owner = basesGetBaseOwner(&g->serverSim.sim.bs, i);
        BYTE shellsAmt, minesAmt, armourAmt;
        basesGetStats(&g->serverSim.sim.bs, i, &shellsAmt, &minesAmt, &armourAmt);
        if (owner == NEUTRAL || armourAmt <= MIN_ARMOUR_CAPTURE) return false;
        if (i == 1) {
            first = owner;
        } else if (!playersIsAllie(&g->serverSim.sim.plyrs, owner, first)) {
            return false;
        }
    }

    /* Game is won — check if player 0 (the gym agent) is on the winning team */
    *agentWon = (first == 0) || playersIsAllie(&g->serverSim.sim.plyrs, 0, first);
    return true;
}

static void gymFreeBrainInfo(BrainInfo *bi) {
    free(bi->allies);
    if (bi->base != NULL) free(bi->base);
    free(bi->pillview);
    free(bi->viewdata);
    if (bi->events != NULL) free(bi->events);
    if (bi->message != NULL) {
        free(bi->message->receivers);
        free(bi->message->message);
        free(bi->message);
    }
}

/* ------------------------------------------------------------------ */
/* View rect helpers for multi-view entity gathering                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int left, top, right, bottom; /* tile coords, inclusive */
} ViewRect;

#define MAX_VIEW_RECTS (1 + WBGYM_MAX_PILLBOXES)

static bool pointInAnyRect(int tx, int ty, const ViewRect *rects, int numRects) {
    for (int i = 0; i < numRects; i++) {
        if (tx >= rects[i].left && tx <= rects[i].right &&
            ty >= rects[i].top  && ty <= rects[i].bottom) {
            return true;
        }
    }
    return false;
}

static bool worldInAnyRect(WORLD wx, WORLD wy, const ViewRect *rects, int numRects) {
    int tx = (int)(wx >> 8);
    int ty = (int)(wy >> 8);
    return pointInAnyRect(tx, ty, rects, numRects);
}

/* Helper to clamp an int to [lo, hi] */
static int gymClamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Determine allegiance of a player relative to self */
static int8_t gymGetAllegiance(BYTE playerNum, BYTE selfPlayer, PlayerBitMap alliesBits) {
    if (playerNum == selfPlayer) return WBGYM_ALLEG_SELF;
    if (playerNum == 0xFF) return WBGYM_ALLEG_NEUTRAL;
    if (alliesBits & (1u << playerNum)) return WBGYM_ALLEG_ALLY;
    return WBGYM_ALLEG_ENEMY;
}

/* Determine owner constant for pills/bases */
static uint8_t gymGetOwner(BYTE owner, BYTE selfPlayer, PlayerBitMap alliesBits) {
    if (owner == 0xFF) return WBGYM_OWNER_NEUTRAL;
    if (owner == selfPlayer) return WBGYM_OWNER_SELF;
    if (alliesBits & (1u << owner)) return WBGYM_OWNER_ALLY;
    return WBGYM_OWNER_ENEMY;
}

/* ------------------------------------------------------------------ */
/* gymBuildObs — V3 observation builder                                */
/* ------------------------------------------------------------------ */

static void gymBuildObs(WinBoloGym *g, WinBoloObs *obs) {
    BrainInfo bi;
    memset(obs, 0, sizeof(*obs));

    screenMakeBrainInfoCS(&g->clientSim, &bi, g->needMapInit, aiYes);
    g->needMapInit = FALSE;

    BYTE selfPlayer = (BYTE)bi.player_number;
    PlayerBitMap alliesBits = bi.allies ? *(bi.allies) : 0;
    bool dead = bi.armour > TANK_FULL_ARMOUR;
    int tank_tx = bi.tankx >> 8;
    int tank_ty = bi.tanky >> 8;
    float self_wx = (float)bi.tankx;
    float self_wy = (float)bi.tanky;

    /* ---- Build view rects ---- */
    ViewRect viewRects[MAX_VIEW_RECTS];
    int numViewRects = 0;

    /* Tank view: 29x29 centered on tank tile */
    viewRects[0].left   = tank_tx - 14;
    viewRects[0].top    = tank_ty - 14;
    viewRects[0].right  = tank_tx + 14;
    viewRects[0].bottom = tank_ty + 14;
    numViewRects = 1;

    /* Each owned alive pill: 15x15 centered on pill */
    BYTE np = pillsGetNumPills(&g->serverSim.sim.pb);
    for (BYTE pi = 1; pi <= np && numViewRects < MAX_VIEW_RECTS; pi++) {
        pillbox p;
        pillsGetPill(&g->serverSim.sim.pb, &p, pi);
        if (p.inTank) continue;
        if (p.armour == 0) continue;
        if (p.owner == 0xFF) continue;
        /* Must be owned by self or ally */
        if (p.owner != selfPlayer && !(alliesBits & (1u << p.owner))) continue;
        viewRects[numViewRects].left   = (int)p.x - 7;
        viewRects[numViewRects].top    = (int)p.y - 7;
        viewRects[numViewRects].right  = (int)p.x + 7;
        viewRects[numViewRects].bottom = (int)p.y + 7;
        numViewRects++;
    }

    /* ---- Discrete game events + sound events ---- */
    for (int i = 0; i < bi.num_events; i++) {
        GameEvent *e = &bi.events[i];
        switch (e->type) {
        case EVENT_SOUND_TANK_HIT:
            if (e->data[3] != selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_HIT_DEALT;
            if (e->data[3] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_HIT_RECEIVED;
            /* Sound event */
            if (obs->num_sounds < WBGYM_MAX_SOUNDS) {
                float sx = (float)e->data[1] - (float)tank_tx;
                float sy = (float)e->data[2] - (float)tank_ty;
                /* Always deliver self-hit regardless of distance */
                bool isSelfHit = (e->data[3] == selfPlayer);
                if (isSelfHit || (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f)) {
                    WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                    snd->rx = sx;
                    snd->ry = sy;
                    snd->type = WBGYM_SND_HIT_TANK;
                    snd->allegiance = gymGetAllegiance(e->data[3], selfPlayer, alliesBits);
                }
            }
            break;
        case EVENT_TANK_KILLED:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_KILL;
            if (e->data[1] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_DEATH;
            break;
        case EVENT_PILL_CAPTURED:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_PILL_CAPTURED;
            if (e->data[1] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_PILL_LOST;
            break;
        case EVENT_BASE_CAPTURED:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_BASE_CAPTURED;
            if (e->data[1] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_BASE_LOST;
            break;
        case EVENT_LGM_LOST:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_LGM_LOST;
            break;
        case EVENT_SOUND_SHOOT: {
            /* Skip own shoot sounds */
            if (e->data[3] == selfPlayer) break;
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_SHOOT;
                snd->allegiance = gymGetAllegiance(e->data[3], selfPlayer, alliesBits);
            }
            break;
        }
        case EVENT_SOUND: {
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_GENERIC;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_EXPLOSION: {
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_EXPLOSION;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_MINE_PLACED: {
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_MINE_PLACE;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_SHELL_FIRED: {
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_SHELL_FIRED;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_ASSISTANT_MSG:
            if (e->data[0] == selfPlayer) {
                obs->assistant_msg = e->data[1];
                /* Map assistant messages to game events */
                switch (e->data[1]) {
                case 1: /* ASSIST_MSG_MAN_DEAD */
                    if (obs->num_events < WBGYM_MAX_EVENTS)
                        obs->events[obs->num_events++] = WBGYM_EVENT_ASSIST_MAN_DEAD;
                    break;
                case 2: /* ASSIST_MSG_NO_TREE */
                case 5: /* ASSIST_MSG_INSUFFICIENT_TREES */
                    if (obs->num_events < WBGYM_MAX_EVENTS)
                        obs->events[obs->num_events++] = WBGYM_EVENT_ASSIST_NO_TREE;
                    break;
                case 6: /* ASSIST_MSG_BUILDTANK */
                    if (obs->num_events < WBGYM_MAX_EVENTS)
                        obs->events[obs->num_events++] = WBGYM_EVENT_ASSIST_BUILDTANK;
                    break;
                }
            }
            break;
        default:
            break;
        }
    }

    /* Also pick up assistant_msg from BrainInfo if set */
    if (bi.assistant_msg != 0 && obs->assistant_msg == 0) {
        obs->assistant_msg = bi.assistant_msg;
    }

    /* ---- Terrain grid: 2 layers ---- */
    const TERRAIN *world = bi.theWorld;
    for (int row = 0; row < WBGYM_SPATIAL_SIZE; row++) {
        for (int col = 0; col < WBGYM_SPATIAL_SIZE; col++) {
            int mx = tank_tx - 14 + col;
            int my = tank_ty - 14 + row;
            if (mx >= 0 && mx < 256 && my >= 0 && my < 256 && world != NULL) {
                BYTE raw = world[my * 256 + mx];
                obs->terrain[row][col] = (float)(raw & TERRAIN_MASK) / 15.0f;
                if (raw & TERRAIN_MINE) {
                    obs->mines_map[row][col] = 1.0f;
                }
            }
        }
    }

    /* ---- Entity list: multi-view gathering from server sim ---- */
    uint16_t ne = 0;
    bool tankSeen[MAX_TANKS] = {0};

    /* Shells (linked list) */
    {
        shells sh = g->serverSim.sim.shs;
        while (sh != NULL && ne < WBGYM_MAX_ENTITIES) {
            WORLD sx, sy;
            sx = sh->x;
            sy = sh->y;
            if (worldInAnyRect(sx, sy, viewRects, numViewRects)) {
                WinBoloEntity *ent = &obs->entities[ne++];
                ent->rx = ((float)sx - self_wx) / 256.0f;
                ent->ry = ((float)sy - self_wy) / 256.0f;
                ent->type = WBGYM_ENT_SHELL;
                ent->allegiance = gymGetAllegiance(sh->owner, selfPlayer, alliesBits);
                ent->direction = (float)sh->angle / 256.0f;
                ent->speed = 0.0f;
                ent->strength = (float)sh->length / 8.0f;
                ent->flags = 0;
                ent->id = 0xFF;
            }
            sh = sh->next;
        }
    }

    /* Tanks (array, all players) */
    for (BYTE p = 0; p < MAX_TANKS; p++) {
        if (!g->serverSim.playerConnected[p]) continue;
        tank *tk = &g->serverSim.sim.tanks[p];
        WORLD tx_w, ty_w;
        tankGetWorld(tk, &tx_w, &ty_w);
        BYTE armour = tankGetArmour(tk);
        bool tankDead = armour > TANK_FULL_ARMOUR;

        if (worldInAnyRect(tx_w, ty_w, viewRects, numViewRects) && ne < WBGYM_MAX_ENTITIES) {
            tankSeen[p] = true;
            WinBoloEntity *ent = &obs->entities[ne++];
            ent->rx = ((float)tx_w - self_wx) / 256.0f;
            ent->ry = ((float)ty_w - self_wy) / 256.0f;
            ent->type = WBGYM_ENT_TANK;
            ent->allegiance = gymGetAllegiance(p, selfPlayer, alliesBits);
            ent->direction = (float)tankGetAngle(tk) / 256.0f;
            ent->speed = (float)tankGetSpeed(tk) / 128.0f;
            ent->strength = tankDead ? 0.0f : (float)armour / 40.0f;
            ent->flags = 0;
            if (tankIsOnBoat(tk)) ent->flags |= WBGYM_FLAG_IN_BOAT;
            if (p == selfPlayer) ent->flags |= WBGYM_FLAG_IS_SELF;
            if (tankDead) ent->flags |= WBGYM_FLAG_DEAD;
            if (tankIsNewTank(tk)) ent->flags |= WBGYM_FLAG_DEAD; /* new tank = just died */
            /* Hidden check: use utilIsTankInTrees for non-self tanks */
            if (p != selfPlayer) {
                if (utilIsTankInTrees(&g->serverSim.sim.mp, &g->serverSim.sim.pb,
                                     &g->serverSim.sim.bs, tx_w, ty_w))
                    ent->flags |= WBGYM_FLAG_HIDDEN;
            } else if (bi.hidden) {
                ent->flags |= WBGYM_FLAG_HIDDEN;
            }
            ent->id = p;
        }
    }

    /* LGMs (array, all players) */
    for (BYTE p = 0; p < MAX_TANKS; p++) {
        if (!g->serverSim.playerConnected[p]) continue;
        lgm *lg = &g->serverSim.sim.lgmen[p];
        BYTE lgmState = lgmGetBrainState(lg);

        if (lgmState == LGM_BRAIN_INTANK) continue; /* in tank, skip */

        WORLD lx = lgmGetWX(lg);
        WORLD ly = lgmGetWY(lg);

        /* LGM_BRAIN_DEAD means parachuting */
        bool isParachuting = (lgmState == LGM_BRAIN_DEAD);

        if (worldInAnyRect(lx, ly, viewRects, numViewRects) && ne < WBGYM_MAX_ENTITIES) {
            WinBoloEntity *ent = &obs->entities[ne++];
            ent->rx = ((float)lx - self_wx) / 256.0f;
            ent->ry = ((float)ly - self_wy) / 256.0f;
            ent->type = isParachuting ? WBGYM_ENT_PARACHUTE : WBGYM_ENT_LGM;
            ent->allegiance = gymGetAllegiance(p, selfPlayer, alliesBits);
            ent->direction = (float)lgmGetDir(lg, &g->serverSim.sim.tanks[p]) / 256.0f;
            ent->speed = 0.0f;
            ent->strength = 0.0f;
            ent->flags = 0;
            if (p == selfPlayer) ent->flags |= WBGYM_FLAG_IS_SELF;
            if (isParachuting) ent->flags |= WBGYM_FLAG_DEAD;
            ent->id = p;
        }
    }

    /* Explosions (linked list) */
    {
        explosions ex = g->serverSim.sim.expl;
        while (ex != NULL && ne < WBGYM_MAX_ENTITIES) {
            int etx = (int)ex->mx;
            int ety = (int)ex->my;
            if (pointInAnyRect(etx, ety, viewRects, numViewRects)) {
                WinBoloEntity *ent = &obs->entities[ne++];
                /* Explosion position: tile + pixel offset for sub-tile precision */
                float ewx = ((float)etx * 256.0f + (float)ex->px);
                float ewy = ((float)ety * 256.0f + (float)ex->py);
                ent->rx = (ewx - self_wx) / 256.0f;
                ent->ry = (ewy - self_wy) / 256.0f;
                ent->type = WBGYM_ENT_EXPLOSION;
                ent->allegiance = WBGYM_ALLEG_NEUTRAL;
                ent->direction = 0.0f;
                ent->speed = 0.0f;
                ent->strength = (float)ex->length / 8.0f;
                ent->flags = 0;
                ent->id = 0xFF;
            }
            ex = ex->next;
        }
    }

    /* Pillboxes (global — not view-gated) */
    np = pillsGetNumPills(&g->serverSim.sim.pb);
    for (BYTE pi = 1; pi <= np && ne < WBGYM_MAX_ENTITIES; pi++) {
        pillbox p;
        pillsGetPill(&g->serverSim.sim.pb, &p, pi);
        WinBoloEntity *ent = &obs->entities[ne++];
        float pwx = (float)p.x * 256.0f + 128.0f; /* center of tile */
        float pwy = (float)p.y * 256.0f + 128.0f;
        ent->rx = (pwx - self_wx) / 256.0f;
        ent->ry = (pwy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_PILLBOX;
        ent->allegiance = (p.owner == 0xFF) ? WBGYM_ALLEG_NEUTRAL
                        : gymGetAllegiance(p.owner, selfPlayer, alliesBits);
        ent->direction = 0.0f;
        ent->speed = 0.0f;
        ent->strength = (float)p.armour / 15.0f;
        ent->flags = 0;
        if (p.inTank) ent->flags |= WBGYM_FLAG_IN_TANK;
        ent->id = pi - 1; /* 0-based index */
    }

    /* Bases (global — not view-gated) */
    BYTE nb = basesGetNumBases(&g->serverSim.sim.bs);
    for (BYTE bsi = 1; bsi <= nb && ne < WBGYM_MAX_ENTITIES; bsi++) {
        base b;
        basesGetBase(&g->serverSim.sim.bs, &b, bsi);
        WinBoloEntity *ent = &obs->entities[ne++];
        float bwx = (float)b.x * 256.0f + 128.0f;
        float bwy = (float)b.y * 256.0f + 128.0f;
        ent->rx = (bwx - self_wx) / 256.0f;
        ent->ry = (bwy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_BASE;
        ent->allegiance = (b.owner == 0xFF) ? WBGYM_ALLEG_NEUTRAL
                        : gymGetAllegiance(b.owner, selfPlayer, alliesBits);
        ent->direction = 0.0f;
        ent->speed = 0.0f;
        /* Base armour from stats */
        {
            BYTE shellsAmt, minesAmt, armourAmt;
            basesGetStats(&g->serverSim.sim.bs, bsi, &shellsAmt, &minesAmt, &armourAmt);
            ent->strength = (float)armourAmt / 90.0f;
        }
        ent->flags = 0;
        ent->id = bsi - 1; /* 0-based index */
    }

    obs->num_entities = ne;

    /* ---- Scalars ---- */
    {
        unsigned armor = dead ? 0 : (unsigned)bi.armour;
        float dir_rad = (float)bi.direction * (2.0f * 3.14159265f / 256.0f);

        obs->scalar[0]  = (float)armor / 40.0f;
        obs->scalar[1]  = (float)bi.shells / 40.0f;
        obs->scalar[2]  = (float)bi.mines / 40.0f;
        obs->scalar[3]  = (float)bi.trees / 40.0f;
        obs->scalar[4]  = (float)bi.speed / 128.0f;
        obs->scalar[5]  = sinf(dir_rad);
        obs->scalar[6]  = cosf(dir_rad);
        obs->scalar[7]  = (float)bi.reload / 15.0f;
        obs->scalar[8]  = bi.inboat ? 1.0f : 0.0f;
        obs->scalar[9]  = bi.carriedpills > 0 ? 1.0f : 0.0f;
        obs->scalar[10] = (float)bi.carriedpills / 16.0f;
        obs->scalar[11] = dead ? 1.0f : 0.0f;

        int self_pills = 0, enemy_pills = 0, ally_pills = 0, total_pills = 0;
        int self_bases = 0, ally_bases = 0, total_bases = 0;
        np = pillsGetNumPills(&g->serverSim.sim.pb);
        total_pills = np;
        for (BYTE pi = 1; pi <= np; pi++) {
            pillbox p;
            pillsGetPill(&g->serverSim.sim.pb, &p, pi);
            if (p.owner == 0xFF) continue;
            if (p.owner == selfPlayer) self_pills++;
            else if (alliesBits & (1u << p.owner)) ally_pills++;
            else enemy_pills++;
        }
        nb = basesGetNumBases(&g->serverSim.sim.bs);
        total_bases = nb;
        for (BYTE bsi = 1; bsi <= nb; bsi++) {
            base b;
            basesGetBase(&g->serverSim.sim.bs, &b, bsi);
            if (b.owner == 0xFF) continue;
            if (b.owner == selfPlayer) self_bases++;
            else if (alliesBits & (1u << b.owner)) ally_bases++;
        }
        float tp = total_pills > 0 ? (float)total_pills : 1.0f;
        float tb = total_bases > 0 ? (float)total_bases : 1.0f;
        obs->scalar[12] = (float)self_pills / tp;
        obs->scalar[13] = (float)enemy_pills / tp;
        obs->scalar[14] = (float)ally_pills / tp;
        obs->scalar[15] = (float)self_bases / tb;
        obs->scalar[16] = (float)ally_bases / tb;
        obs->scalar[17] = (float)tank_tx / 256.0f;
        obs->scalar[18] = (float)tank_ty / 256.0f;

        /* New scalars 19-25 */
        obs->scalar[19] = (float)bi.gunrange / 14.0f;
        obs->scalar[20] = bi.hidden ? 1.0f : 0.0f;
        obs->scalar[21] = bi.newtank ? 1.0f : 0.0f;
        obs->scalar[22] = bi.tankobstructed ? 1.0f : 0.0f;

        /* man_status: 0=in tank, 0.33=outside working, 0.66=parachuting, 1=dead */
        {
            BYTE ms = bi.man_status;
            if (ms == LGM_BRAIN_INTANK) {
                obs->scalar[23] = 0.0f;
            } else if (ms == LGM_BRAIN_DEAD) {
                /* Check if actually parachuting (isDead but tank alive = parachuting) */
                lgm *lg = &g->serverSim.sim.lgmen[selfPlayer];
                if ((*lg)->isDead && tankGetArmour(&g->serverSim.sim.tanks[selfPlayer]) <= TANK_FULL_ARMOUR) {
                    obs->scalar[23] = 0.66f; /* parachuting */
                } else {
                    obs->scalar[23] = 1.0f; /* actually dead */
                }
            } else {
                obs->scalar[23] = 0.33f; /* outside working */
            }
        }

        /* man_obstructed: 0/0.5/1 */
        obs->scalar[24] = (float)bi.manobstructed / 2.0f;

        /* death_wait */
        obs->scalar[25] = (float)tankGetDeathWait(&g->serverSim.sim.tanks[selfPlayer]) / 255.0f;
    }

    /* ---- LGM state ---- */
    {
        obs->man_rx = ((float)bi.man_x - self_wx) / 256.0f;
        obs->man_ry = ((float)bi.man_y - self_wy) / 256.0f;
        obs->man_direction = (float)bi.man_direction / 256.0f;
    }

    /* Full-precision tank position in tile units */
    obs->tank_x = (float)bi.tankx / 256.0f;
    obs->tank_y = (float)bi.tanky / 256.0f;

    /* ---- Pillbox list ---- */
    {
        np = pillsGetNumPills(&g->serverSim.sim.pb);
        for (BYTE pi = 1; pi <= np && obs->num_pillboxes < WBGYM_MAX_PILLBOXES; pi++) {
            pillbox p;
            pillsGetPill(&g->serverSim.sim.pb, &p, pi);
            WinBoloPillObs *po = &obs->pillboxes[obs->num_pillboxes];
            po->tx = p.x;
            po->ty = p.y;
            po->armor = p.armour;
            po->owner = gymGetOwner(p.owner, selfPlayer, alliesBits);
            obs->num_pillboxes++;
        }
    }

    /* ---- Base list ---- */
    {
        nb = basesGetNumBases(&g->serverSim.sim.bs);
        for (BYTE bsi = 1; bsi <= nb && obs->num_bases < WBGYM_MAX_BASES; bsi++) {
            base b;
            basesGetBase(&g->serverSim.sim.bs, &b, bsi);
            WinBoloBaseObs *bo = &obs->bases[obs->num_bases];
            bo->tx = b.x;
            bo->ty = b.y;
            bo->owner = gymGetOwner(b.owner, selfPlayer, alliesBits);
            /* Only expose stocks for friendly/allied bases */
            if (bo->owner == WBGYM_OWNER_SELF || bo->owner == WBGYM_OWNER_ALLY) {
                BYTE shellsAmt, minesAmt, armourAmt;
                basesGetStats(&g->serverSim.sim.bs, bsi, &shellsAmt, &minesAmt, &armourAmt);
                bo->shells = shellsAmt;
                bo->mines = minesAmt;
                bo->armour = armourAmt;
            }
            obs->num_bases++;
        }
    }

    obs->dead = dead ? 1 : 0;
    obs->tick = (uint32_t)g->gameTickCount;

    /* Check game-over (same logic as -quitonwin) */
    bool agentWon = false;
    if (gymCheckGameWin(g, &agentWon)) {
        obs->game_over = 1;
        obs->game_won = agentWon ? 1 : 0;
    }

    gymFreeBrainInfo(&bi);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

WBGYM_API WinBoloGym *winbolo_create(const char *map_path, int game_type) {
    WinBoloGym *g = (WinBoloGym *)calloc(1, sizeof(WinBoloGym));
    if (g == NULL) return NULL;

    g->gameMode = (gameType)game_type;
    guiMessageSetHandler(gymMessageHandler);

    if (!serverSimCreate(&g->serverSim, (char *)map_path, g->gameMode,
                         false, 0, UNLIMITED_GAME_TIME)) {
        free(g);
        return NULL;
    }

    /* Cache compressed map for fast resets */
    BYTE tempMap[65536];
    g->cachedMapLen = serverSimGetCompressedMap(&g->serverSim, tempMap);
    if (g->cachedMapLen <= 0) {
        serverSimDestroy(&g->serverSim);
        free(g);
        return NULL;
    }
    g->cachedMap = (BYTE *)malloc(g->cachedMapLen);
    memcpy(g->cachedMap, tempMap, g->cachedMapLen);

    gymSetupGame(g);

    return g;
}

WBGYM_API void winbolo_destroy(WinBoloGym *game) {
    if (game == NULL) return;
    gymTeardownGame(game);
    serverSimDestroy(&game->serverSim);
    free(game->cachedMap);
    free(game);
}

WBGYM_API void winbolo_step(WinBoloGym *game, const WinBoloAction *action, WinBoloObs *obs_out) {
    if (game == NULL || obs_out == NULL) return;

    /* Build input packet from action */
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick = game->simTickCounter;
    pkt.playerNum = 0;

    if (action != NULL) {
        if (action->accel > 0)  pkt.buttons |= INPUT_BTN_ACCEL;
        if (action->accel < 0)  pkt.buttons |= INPUT_BTN_DECEL;
        if (action->turn < 0)   pkt.buttons |= INPUT_BTN_LEFT;
        if (action->turn > 0)   pkt.buttons |= INPUT_BTN_RIGHT;
        if (action->shoot)      pkt.actions |= INPUT_ACTION_FIRE;
        if (action->lay_mine)   pkt.actions |= INPUT_ACTION_LAY_MINE;

        /* Build action: convert relative coords to absolute */
        pkt.buildAction = (uint8_t)action->build_action;
        if (action->build_action > 0) {
            WORLD twx, twy;
            tankGetWorld(&game->serverSim.sim.tanks[0], &twx, &twy);
            int ttx = (int)(twx >> 8);
            int tty = (int)(twy >> 8);
            pkt.buildX = (uint8_t)gymClamp(ttx + action->build_rx, 0, 255);
            pkt.buildY = (uint8_t)gymClamp(tty + action->build_ry, 0, 255);
        }

        /* Gun range adjustment */
        if (action->gun_range_adjust > 0)  pkt.gunsightAdj = 1;
        if (action->gun_range_adjust < 0)  pkt.gunsightAdj = (uint8_t)-1;
    }

    /* Game tick */
    clientSimGameTick(&game->clientSim, &pkt, FALSE);
    game->transport.sendInput(game->transport.ctx, &pkt);
    game->transport.tick(game->transport.ctx);
    gymSyncSnapshot(game);
    clientSimDisplayTick(&game->clientSim, FALSE);
    game->simTickCounter++;
    game->gameTickCount++;

    /* Keys tick — replay held buttons */
    InputPacket keysPkt;
    memset(&keysPkt, 0, sizeof(keysPkt));
    keysPkt.tick = game->simTickCounter;
    keysPkt.playerNum = 0;
    keysPkt.buttons = pkt.buttons;

    clientSimKeysTick(&game->clientSim, &keysPkt);
    game->transport.sendInput(game->transport.ctx, &keysPkt);
    game->transport.tick(game->transport.ctx);
    gymSyncSnapshot(game);
    game->simTickCounter++;

    gymBuildObs(game, obs_out);
}

WBGYM_API void winbolo_reset(WinBoloGym *game, WinBoloObs *obs_out) {
    if (game == NULL || obs_out == NULL) return;

    gymTeardownGame(game);
    gymSetupGame(game);
    gymBuildObs(game, obs_out);
}

WBGYM_API int winbolo_obs_size(void) {
    return (int)sizeof(WinBoloObs);
}

WBGYM_API int winbolo_action_size(void) {
    return (int)sizeof(WinBoloAction);
}

WBGYM_API void winbolo_step_batch(
    WinBoloGym **games,
    const WinBoloAction *actions,
    WinBoloObs *obs_out,
    int count
) {
    for (int i = 0; i < count; i++) {
        winbolo_step(games[i], &actions[i], &obs_out[i]);
    }
}
