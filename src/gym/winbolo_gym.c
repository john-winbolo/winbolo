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
 *********************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "winbolo_gym.h"

#include "../bolo/global.h"
#include "../bolo/screen.h"
#include "../bolo/client_sim.h"
#include "../bolo/client_sim_control.h"
#include "../bolo/control_event.h"
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
#include "../bolo/brain_worldsim.h"

/* Required by the engine — stub for library mode */
bool isInMenu = FALSE;

/* Per-instance game state */
struct WinBoloGym {
    ServerSim   serverSim;
    ClientSim   clientSim;
    Transport   transport;
    SubscriberHandle controlSub;

    BYTE       *cachedMap;
    int         cachedMapLen;

    gameType    gameMode;

    uint32_t    simTickCounter;
    int         gameTickCount;
    bool        needMapInit;

    RewardState rewardState;
    bool        rewardsEnabled;  /* true once winbolo_set_reward_weights called */

    /* Pre-allocated buffers for gymMakeBrainInfo (avoids malloc per tick) */
    PlayerBitMap cachedAllies;
    GameEvent    cachedEvents[MAX_BRAIN_EVENTS];
    int          cachedEventCount;
};

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void gymMessageHandler(const char *message, const char *title) {
    (void)message; (void)title;
}

static void gymDeliverControl(void *ctx, const ControlEvent *evt) {
    clientSimApplyControl((ClientSim *)ctx, evt);
}

/* Accumulate server events into pre-allocated cache.
 * Called after each serverSimTick (before the next tick clears them).
 * Filters to the same event types that the snapshot sync path delivered
 * to brainEvents, preserving identical observation behavior. */
static void gymBufferServerEvents(WinBoloGym *g) {
    ServerSim *ss = &g->serverSim;
    for (int i = 0; i < ss->eventCount && g->cachedEventCount < MAX_BRAIN_EVENTS; i++) {
        switch (ss->events[i].type) {
        case EVENT_SOUND:
        case EVENT_SOUND_SHOOT:
        case EVENT_SOUND_TANK_HIT:
        case EVENT_PILL_CAPTURED:
        case EVENT_BASE_CAPTURED:
        case EVENT_TANK_KILLED:
        case EVENT_LGM_LOST:
        case EVENT_PLAYER_LEAVE:
        case EVENT_PILL_UPDATE:
        case EVENT_BASE_UPDATE:
        case EVENT_ASSISTANT_MSG:
            g->cachedEvents[g->cachedEventCount++] = ss->events[i];
            break;
        default:
            break;
        }
    }
}

static void gymSyncSnapshot(WinBoloGym *g) {
    SnapshotHeader snapHdr;
    TankSnapshot snapTanks[MAX_TANKS];
    ShellSnapshot snapShells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot snapTkExplosions[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot snapBases[MAX_SNAPSHOT_BASES];
    PillSnapshot snapPills[MAX_SNAPSHOT_PILLS];
    GameEvent snapEvents[MAX_SNAPSHOT_EVENTS];

    if (g->transport.getSnapshot(g->transport.ctx, 0,
                                  &snapHdr, snapTanks, MAX_TANKS,
                                  snapShells, MAX_SNAPSHOT_SHELLS,
                                  snapTkExplosions, MAX_SNAPSHOT_TK_EXPLOSIONS,
                                  snapBases, MAX_SNAPSHOT_BASES,
                                  snapPills, MAX_SNAPSHOT_PILLS,
                                  snapEvents, MAX_SNAPSHOT_EVENTS)) {
        clientSimSyncFromSnapshot(&g->clientSim, &snapHdr,
                                  snapTanks, snapHdr.tankCount,
                                  snapShells, snapHdr.shellCount,
                                  snapTkExplosions, snapHdr.tkExplosionCount,
                                  snapBases, snapHdr.baseCount,
                                  snapPills, snapHdr.pillCount,
                                  snapEvents, snapHdr.reliableEventCount, 0);
    }
}

static void gymSetupGame(WinBoloGym *g) {
    g->serverSim.lobbyEnabled = false;
    serverSimStartGame(&g->serverSim);
    serverSimAddPlayer(&g->serverSim, 0, "GymAgent", false);
    g->serverSim.sim.viewPlayer = 0;

    g->transport = transportLocalCreate(&g->serverSim, 0);

    screenLoadCompressedMapCS(&g->clientSim, g->cachedMap, g->cachedMapLen,
                              "Gym", g->gameMode, false, 0,
                              UNLIMITED_GAME_TIME, "GymAgent", 0, FALSE);
    screenSetAiTypeCS(&g->clientSim, aiYes);
    gymSyncSnapshot(g);
    screenNetSetupTankGoCS(&g->clientSim);

    /* Register the gym client as a control-event subscriber. Placed after
     * screenLoadCompressedMapCS (which calls clientSimCreate) so myPlayerNum
     * is initialized to 0 — matching the gym agent's slot — before sync's
     * self-skip runs. */
    g->controlSub = serverSimRegisterSubscriber(&g->serverSim,
                                                gymDeliverControl,
                                                &g->clientSim);

    g->simTickCounter = 0;
    g->gameTickCount = 0;
    g->needMapInit = TRUE;
    g->cachedEventCount = 0;

    /* Reset reward state (preserve weights and rewardsEnabled) */
    {
        float saved_weights[WBGYM_NUM_REWARD_COMPONENTS];
        bool saved_enabled = g->rewardsEnabled;
        memcpy(saved_weights, g->rewardState.weights, sizeof(saved_weights));
        memset(&g->rewardState, 0, sizeof(RewardState));
        memcpy(g->rewardState.weights, saved_weights, sizeof(saved_weights));
        g->rewardsEnabled = saved_enabled;
        g->rewardState.last_spike_tick = -100;
    }
}

static void gymTeardownGame(WinBoloGym *g) {
    serverSimUnregisterSubscriber(&g->serverSim, g->controlSub);
    g->controlSub = SUBSCRIBER_HANDLE_INVALID;
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
/* gymMakeBrainInfo — lightweight BrainInfo for gym (no malloc)        */
/* ------------------------------------------------------------------ */

static void gymMakeBrainInfo(WinBoloGym *g, BrainInfo *bi) {
    GameSim *sim = &g->serverSim.sim;

    memset(bi, 0, sizeof(*bi));

    bi->player_number = 0; /* gym agent is always player 0 */
    bi->allies = &g->cachedAllies;
    g->cachedAllies = playersGetAlliesBitMap(&sim->plyrs, 0);

    /* Tank state — read directly from server sim (authoritative) */
    tankGetWorld(&sim->tanks[0], &bi->tankx, &bi->tanky);
    bi->direction = tankGet256Dir(&sim->tanks[0]);
    bi->speed = (BYTE)(tankGetSpeed(&sim->tanks[0]) * 4);
    bi->inboat = tankIsOnBoat(&sim->tanks[0]);
    bi->hidden = utilIsTankInTrees(&sim->mp, &sim->pb, &sim->bs, bi->tankx, bi->tanky);
    tankGetStats(&sim->tanks[0], &bi->shells, &bi->mines, &bi->armour, &bi->trees);
    bi->gunrange = tankGetGunsightLength(&sim->tanks[0]);
    bi->reload = tankGetReloadTime(&sim->tanks[0]);

    /* Carried pills */
    {
        BYTE numPb = pillsGetNumPills(&sim->pb);
        BYTE carried = 0;
        for (BYTE pi = 0; pi < numPb; pi++) {
            if ((*sim->pb).item[pi].inTank && (*sim->pb).item[pi].owner == 0)
                carried++;
        }
        bi->carriedpills = carried;
    }

    /* LGM */
    bi->man_status = lgmGetBrainState(&sim->lgmen[0]);
    bi->man_direction = lgmGetDir(&sim->lgmen[0], &sim->tanks[0]);
    bi->man_x = lgmGetWX(&sim->lgmen[0]);
    bi->man_y = lgmGetWY(&sim->lgmen[0]);
    bi->manobstructed = lgmGetBrainObstructed(&sim->lgmen[0]);

    /* Server tick and assistant message */
    bi->server_tick = g->serverSim.tick;

    /* Events — read directly from server event buffer into pre-allocated cache.
     * Called once per step AFTER both ticks, with events accumulated by
     * gymBufferServerEvents() between ticks. */
    bi->events = g->cachedEventCount > 0 ? g->cachedEvents : NULL;
    bi->num_events = (u_short)g->cachedEventCount;

    /* Check for assistant messages in events */
    for (int i = 0; i < g->cachedEventCount; i++) {
        if (g->cachedEvents[i].type == EVENT_ASSISTANT_MSG &&
            g->cachedEvents[i].data[0] == 0) {
            bi->assistant_msg = g->cachedEvents[i].data[1];
        }
    }

    /* Terrain: not set here — gymBuildObs reads server map directly */
    bi->theWorld = NULL;
}

/* ------------------------------------------------------------------ */
/* gymBuildObs — V3 observation builder                                */
/* ------------------------------------------------------------------ */

static void gymBuildObs(WinBoloGym *g, WinBoloObs *obs) {
    BrainInfo bi;
    memset(obs, 0, sizeof(*obs));

    gymMakeBrainInfo(g, &bi);
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
            float sx = (float)e->data[0] - (float)tank_tx;
            float sy = (float)e->data[1] - (float)tank_ty;
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

    /* ---- Terrain grid: 2 layers (read directly from server map) ---- */
    for (int row = 0; row < WBGYM_SPATIAL_SIZE; row++) {
        for (int col = 0; col < WBGYM_SPATIAL_SIZE; col++) {
            int mx = tank_tx - 14 + col;
            int my = tank_ty - 14 + row;
            if (mx >= 0 && mx < 256 && my >= 0 && my < 256) {
                BYTE raw = mapGetPos(&g->serverSim.sim.mp, (BYTE)mx, (BYTE)my);
                BYTE terrain = raw;
                if (terrain >= MINE_START && terrain <= MINE_END) {
                    terrain -= MINE_SUBTRACT;
                } else if (terrain == DEEP_SEA) {
                    terrain = BDEEPSEA;
                }
                obs->terrain[row][col] = (float)(terrain & TERRAIN_MASK) / 15.0f;
                if (minesExistPos(&g->serverSim.sim.mns, &g->serverSim.sim.mp,
                                  (BYTE)mx, (BYTE)my)) {
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
        obs->scalar[21] = tankIsNewTank(&g->serverSim.sim.tanks[selfPlayer]) ? 1.0f : 0.0f;
        obs->scalar[22] = tankIsObstructed(&g->serverSim.sim.tanks[selfPlayer]) ? 1.0f : 0.0f;

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

    /* No gymFreeBrainInfo needed — gymMakeBrainInfo uses pre-allocated buffers */
}

/* ------------------------------------------------------------------ */
/* Reward computation                                                  */
/* ------------------------------------------------------------------ */

static int gymCountEvents(const WinBoloObs *obs, uint8_t event_type) {
    int count = 0;
    for (int i = 0; i < obs->num_events; i++) {
        if (obs->events[i] == event_type) count++;
    }
    return count;
}

static bool gymIsOnOwnBase(float tank_x, float tank_y, const WinBoloObs *obs) {
    int ttx = (int)tank_x;
    int tty = (int)tank_y;
    for (int i = 0; i < obs->num_bases; i++) {
        if (obs->bases[i].owner == WBGYM_OWNER_SELF &&
            (int)obs->bases[i].tx == ttx && (int)obs->bases[i].ty == tty)
            return true;
    }
    return false;
}

static void gymComputeRewardsMut(WinBoloGym *g, WinBoloObs *obs,
                                  const WinBoloAction *action) {
    RewardState *rs = &g->rewardState;
    const float *w = rs->weights;
    float *comp = obs->reward_components;
    memset(comp, 0, sizeof(obs->reward_components));
    obs->reward = 0.0f;

    float tank_x = obs->tank_x;
    float tank_y = obs->tank_y;
    bool dead = obs->dead != 0;
    bool alive = !dead;
    bool game_won = obs->game_won != 0;
    bool has_prev = rs->has_prev;

    /* Current scalars shorthand */
    const float *sc = obs->scalar;
    const float *psc = rs->prev_scalars;
    float lgm_status = sc[WBGYM_S_LGM_STATUS];

    /* ── CATEGORY 1: Survival ── */
    if (w[RC_DEATH] != 0.0f || w[RC_DEATH_WITH_PILLS] != 0.0f ||
        w[RC_DEATH_WITH_LGM_OUT] != 0.0f || w[RC_SURVIVAL_TICK] != 0.0f) {

        float death_count = (float)gymCountEvents(obs, WBGYM_EVENT_DEATH);
        bool death_occurred = death_count > 0;

        comp[RC_DEATH] = death_count;
        comp[RC_DEATH_WITH_PILLS] = death_occurred ? sc[WBGYM_S_PILL_COUNT] * 16.0f : 0.0f;

        bool prev_lgm_outside = (rs->prev_lgm_status > WBGYM_LGM_OUTSIDE_LO) &&
                                (rs->prev_lgm_status < WBGYM_LGM_OUTSIDE_HI);
        comp[RC_DEATH_WITH_LGM_OUT] = (death_occurred && prev_lgm_outside) ? 1.0f : 0.0f;
        comp[RC_SURVIVAL_TICK] = alive ? 1.0f : 0.0f;
    }

    /* ── CATEGORY 2: Combat ── */
    if (w[RC_HIT_DEALT] != 0.0f || w[RC_HIT_RECEIVED] != 0.0f ||
        w[RC_KILL] != 0.0f || w[RC_KILL_CARRIER] != 0.0f ||
        w[RC_SHOT_FIRED] != 0.0f || w[RC_SHOT_ACCURACY] != 0.0f) {

        float hit_dealt = (float)gymCountEvents(obs, WBGYM_EVENT_HIT_DEALT);
        float hit_received = (float)gymCountEvents(obs, WBGYM_EVENT_HIT_RECEIVED);
        float kill_count = (float)gymCountEvents(obs, WBGYM_EVENT_KILL);

        comp[RC_HIT_DEALT] = hit_dealt;
        comp[RC_HIT_RECEIVED] = hit_received;
        comp[RC_KILL] = kill_count;

        /* kill_carrier: kill happened AND own pill frac went up */
        bool pill_frac_up = has_prev &&
            (sc[WBGYM_S_OWN_PILL_FRAC] > psc[WBGYM_S_OWN_PILL_FRAC] + 0.01f);
        comp[RC_KILL_CARRIER] = (kill_count > 0 && pill_frac_up) ? 1.0f : 0.0f;

        /* shot_fired: only count if reload was ready on prev tick */
        float shot_this_tick = 0.0f;
        if (has_prev && psc[WBGYM_S_RELOAD] < 0.07f && action != NULL) {
            shot_this_tick = action->shoot ? 1.0f : 0.0f;
        }
        comp[RC_SHOT_FIRED] = shot_this_tick;

        /* Update rolling shot/hit windows */
        int widx = rs->window_idx % WBGYM_SHOT_WINDOW;
        rs->shot_window[widx] = shot_this_tick;
        rs->hit_window[widx] = hit_dealt < 1.0f ? hit_dealt : 1.0f;
        rs->window_idx++;

        float total_shots = 0.0f, total_hits = 0.0f;
        for (int i = 0; i < WBGYM_SHOT_WINDOW; i++) {
            total_shots += rs->shot_window[i];
            total_hits += rs->hit_window[i];
        }
        if (total_shots >= 5.0f) {
            comp[RC_SHOT_ACCURACY] = total_hits / total_shots;
        }
    }

    /* ── CATEGORY 3: Pillbox Control ── */
    if (w[RC_PILL_CAPTURED] != 0.0f || w[RC_PILL_LOST] != 0.0f ||
        w[RC_PILL_DESTROYED] != 0.0f || w[RC_OWN_PILL_FRAC_DELTA] != 0.0f ||
        w[RC_PILL_PLACEMENT_QUAL] != 0.0f || w[RC_PILL_HEATED_TACTICAL] != 0.0f ||
        w[RC_PILL_HIT] != 0.0f || w[RC_SHOOT_AT_PILL] != 0.0f) {

        comp[RC_PILL_CAPTURED] = (float)gymCountEvents(obs, WBGYM_EVENT_PILL_CAPTURED);
        comp[RC_PILL_LOST] = (float)gymCountEvents(obs, WBGYM_EVENT_PILL_LOST);

        /* pill_destroyed + pill_hit: track armor damage to non-friendly pills */
        if (has_prev) {
            float destroyed = 0.0f;
            float hits = 0.0f;
            int np = obs->num_pillboxes < rs->prev_pill_count ?
                     obs->num_pillboxes : rs->prev_pill_count;
            for (int i = 0; i < np; i++) {
                float prev_owner = rs->prev_pills[i][2];
                float prev_armor = rs->prev_pills[i][3];
                float curr_armor = (float)obs->pillboxes[i].armor;
                bool prev_not_friendly = (prev_owner != WBGYM_OWNER_SELF) &&
                                         (prev_owner != WBGYM_OWNER_ALLY);
                if (prev_not_friendly && prev_armor > curr_armor) {
                    hits += (prev_armor - curr_armor);
                    if (curr_armor == 0) destroyed += 1.0f;
                }
            }
            comp[RC_PILL_DESTROYED] = destroyed;
            comp[RC_PILL_HIT] = hits;

            comp[RC_OWN_PILL_FRAC_DELTA] =
                sc[WBGYM_S_OWN_PILL_FRAC] - psc[WBGYM_S_OWN_PILL_FRAC];
        }

        /* pill_placement_quality: newly-owned pill near own base */
        if (has_prev) {
            float quality = 0.0f;
            for (int pi = 0; pi < obs->num_pillboxes && pi < rs->prev_pill_count; pi++) {
                if (rs->prev_pills[pi][2] != WBGYM_OWNER_SELF &&
                    obs->pillboxes[pi].owner == WBGYM_OWNER_SELF) {
                    /* Check distance to any own base */
                    float px = (float)obs->pillboxes[pi].tx;
                    float py = (float)obs->pillboxes[pi].ty;
                    for (int bi = 0; bi < obs->num_bases; bi++) {
                        if (obs->bases[bi].owner == WBGYM_OWNER_SELF) {
                            float dx = px - (float)obs->bases[bi].tx;
                            float dy = py - (float)obs->bases[bi].ty;
                            if (dx*dx + dy*dy <= 100.0f) {
                                quality += 1.0f;
                                break;
                            }
                        }
                    }
                }
            }
            comp[RC_PILL_PLACEMENT_QUAL] = quality;
        }

        /* pill_heated_tactical: own pill losing armor while enemy nearby */
        if (has_prev) {
            float heated = 0.0f;
            for (int pi = 0; pi < obs->num_pillboxes && pi < rs->prev_pill_count; pi++) {
                if (rs->prev_pills[pi][2] != WBGYM_OWNER_SELF) continue;
                if (obs->pillboxes[pi].owner != WBGYM_OWNER_SELF) continue;
                if (rs->prev_pills[pi][3] <= (float)obs->pillboxes[pi].armor) continue;

                /* Check if any enemy tank entity is near this pill */
                float px = (float)obs->pillboxes[pi].tx;
                float py = (float)obs->pillboxes[pi].ty;
                for (int ei = 0; ei < obs->num_entities; ei++) {
                    const WinBoloEntity *e = &obs->entities[ei];
                    if (e->type != WBGYM_ENT_TANK || e->allegiance != WBGYM_ALLEG_ENEMY)
                        continue;
                    float ex = tank_x + e->rx;
                    float ey = tank_y + e->ry;
                    float dx = px - ex;
                    float dy = py - ey;
                    if (dx*dx + dy*dy < 144.0f) {
                        heated += 1.0f;
                        break;
                    }
                }
            }
            comp[RC_PILL_HEATED_TACTICAL] = heated;
        }

        /* shoot_at_pill: fired a shot while facing a nearby non-owned pill
         * Only on land, with ammo, within tank shooting range (8 tiles) */
        if (alive && has_prev && action != NULL && w[RC_SHOOT_AT_PILL] != 0.0f
            && sc[WBGYM_S_IN_BOAT] < 0.5f && sc[WBGYM_S_SHELLS] > 0.01f) {
            bool shot = (psc[WBGYM_S_RELOAD] < 0.07f) && action->shoot;
            if (shot) {
                float fx = obs->scalar[5];   /* east component of facing */
                float fy = -obs->scalar[6];  /* south component */
                float best_dot = -2.0f;
                float best_dist = 1e9f;
                for (int pi = 0; pi < obs->num_pillboxes; pi++) {
                    if (obs->pillboxes[pi].owner == WBGYM_OWNER_SELF ||
                        obs->pillboxes[pi].owner == WBGYM_OWNER_ALLY)
                        continue;
                    float dx = (float)obs->pillboxes[pi].tx - tank_x;
                    float dy = (float)obs->pillboxes[pi].ty - tank_y;
                    float dist = sqrtf(dx*dx + dy*dy);
                    if (dist < 0.5f || dist > 8.0f) continue;
                    float dot = (fx*dx + fy*dy) / dist;
                    if (dot > best_dot) {
                        best_dot = dot;
                        best_dist = dist;
                    }
                }
                /* Reward if facing pill (dot > 0.7 ~ within 45 deg) and within range */
                if (best_dot > 0.7f) {
                    comp[RC_SHOOT_AT_PILL] = best_dot;
                }
            }
        }
    }

    /* ── CATEGORY 4: Base Control ── */
    if (w[RC_BASE_CAPTURED] != 0.0f || w[RC_BASE_LOST] != 0.0f ||
        w[RC_OWN_BASE_FRAC_DELTA] != 0.0f || w[RC_BASE_RATIO] != 0.0f ||
        w[RC_ALL_BASES_OWNED] != 0.0f) {

        comp[RC_BASE_CAPTURED] = (float)gymCountEvents(obs, WBGYM_EVENT_BASE_CAPTURED);
        comp[RC_BASE_LOST] = (float)gymCountEvents(obs, WBGYM_EVENT_BASE_LOST);

        if (has_prev) {
            comp[RC_OWN_BASE_FRAC_DELTA] =
                sc[WBGYM_S_OWN_BASE_FRAC] - psc[WBGYM_S_OWN_BASE_FRAC];
        }

        float own_base_n = 0, enemy_base_n = 0;
        for (int i = 0; i < obs->num_bases; i++) {
            if (obs->bases[i].owner == WBGYM_OWNER_SELF) own_base_n++;
            else if (obs->bases[i].owner == WBGYM_OWNER_ENEMY) enemy_base_n++;
        }
        float total_be = own_base_n + enemy_base_n;
        comp[RC_BASE_RATIO] = alive ? own_base_n / (total_be > 0 ? total_be : 1.0f) : 0.0f;
        comp[RC_ALL_BASES_OWNED] = game_won ? 1.0f : 0.0f;
    }

    /* ── CATEGORY 5: Builder / LGM ── */
    if (w[RC_LGM_LOST] != 0.0f || w[RC_LGM_PARACHUTING_TICK] != 0.0f ||
        w[RC_ENEMY_LGM_KILLED] != 0.0f || w[RC_SUCCESSFUL_BUILD] != 0.0f ||
        w[RC_FAILED_BUILD] != 0.0f || w[RC_LGM_SENT_DANGEROUS] != 0.0f) {

        comp[RC_LGM_LOST] = (float)gymCountEvents(obs, WBGYM_EVENT_LGM_LOST);

        bool lgm_para = (lgm_status > WBGYM_LGM_PARA_LO) && (lgm_status < WBGYM_LGM_PARA_HI);
        comp[RC_LGM_PARACHUTING_TICK] = (alive && lgm_para) ? 1.0f : 0.0f;

        /* enemy_lgm_killed: check for enemy LGM_LOST sound */
        bool enemy_lgm = false;
        for (int i = 0; i < obs->num_sounds; i++) {
            if (obs->sounds[i].type == WBGYM_SND_LGM_LOST &&
                obs->sounds[i].allegiance == WBGYM_ALLEG_ENEMY) {
                enemy_lgm = true;
                break;
            }
        }
        comp[RC_ENEMY_LGM_KILLED] = enemy_lgm ? 1.0f : 0.0f;

        /* successful_build / failed_build */
        bool prev_lgm_in_tank = rs->prev_lgm_status < WBGYM_LGM_IN_TANK_THRESH;
        bool prev_lgm_outside = (rs->prev_lgm_status > WBGYM_LGM_OUTSIDE_LO) &&
                                (rs->prev_lgm_status < WBGYM_LGM_OUTSIDE_HI);
        bool curr_lgm_in_tank = lgm_status < WBGYM_LGM_IN_TANK_THRESH;
        bool curr_lgm_outside = (lgm_status > WBGYM_LGM_OUTSIDE_LO) &&
                                (lgm_status < WBGYM_LGM_OUTSIDE_HI);

        bool lgm_returned = has_prev && prev_lgm_outside && curr_lgm_in_tank;
        bool lgm_just_sent = has_prev && prev_lgm_in_tank && curr_lgm_outside;

        bool failed_msg = WBGYM_FAILED_BUILD_MSG(obs->assistant_msg);
        comp[RC_SUCCESSFUL_BUILD] = (lgm_returned && !failed_msg) ? 1.0f : 0.0f;
        comp[RC_FAILED_BUILD] = (lgm_returned && failed_msg) ? 1.0f : 0.0f;

        /* lgm_sent_dangerous: building near enemy pill or tank */
        if (action != NULL && action->build_action > 0) {
            float bx = tank_x + (float)action->build_rx;
            float by = tank_y + (float)action->build_ry;
            bool enemy_near = false;

            /* Check enemy pills */
            for (int i = 0; i < obs->num_pillboxes && !enemy_near; i++) {
                if (obs->pillboxes[i].owner != WBGYM_OWNER_ENEMY) continue;
                float dx = bx - (float)obs->pillboxes[i].tx;
                float dy = by - (float)obs->pillboxes[i].ty;
                if (dx*dx + dy*dy < 81.0f) enemy_near = true;
            }
            /* Check enemy tanks */
            for (int i = 0; i < obs->num_entities && !enemy_near; i++) {
                if (obs->entities[i].type != WBGYM_ENT_TANK ||
                    obs->entities[i].allegiance != WBGYM_ALLEG_ENEMY) continue;
                float ex = tank_x + obs->entities[i].rx;
                float ey = tank_y + obs->entities[i].ry;
                float dx = bx - ex;
                float dy = by - ey;
                if (dx*dx + dy*dy < 64.0f) enemy_near = true;
            }
            comp[RC_LGM_SENT_DANGEROUS] = enemy_near ? 1.0f : 0.0f;
        }

        /* Track active build action when LGM just sent */
        if (lgm_just_sent && action != NULL) {
            rs->active_build_action = action->build_action;
        }
    }

    /* ── CATEGORY 6: Resources ── */
    if (w[RC_RESUPPLY_EFFICIENCY] != 0.0f || w[RC_RESUPPLY_CAMPING] != 0.0f ||
        w[RC_TREES_FARMED] != 0.0f || w[RC_AMMO_CONSERVATION] != 0.0f ||
        w[RC_IDLE_PENALTY] != 0.0f || w[RC_RESUPPLY_SHELLS] != 0.0f) {

        bool on_own_base = gymIsOnOwnBase(tank_x, tank_y, obs);

        bool armor_up = has_prev && (sc[WBGYM_S_ARMOR] > psc[WBGYM_S_ARMOR] + 0.01f);
        comp[RC_RESUPPLY_EFFICIENCY] = (armor_up && on_own_base) ? 1.0f : 0.0f;

        bool shells_up = has_prev && (sc[WBGYM_S_SHELLS] > psc[WBGYM_S_SHELLS] + 0.01f);
        comp[RC_RESUPPLY_SHELLS] = (shells_up && on_own_base) ? 1.0f : 0.0f;

        bool full_stock = (sc[WBGYM_S_ARMOR] > 0.99f) &&
                          (sc[WBGYM_S_SHELLS] > 0.99f) &&
                          (sc[WBGYM_S_MINES] > 0.99f);
        comp[RC_RESUPPLY_CAMPING] = (alive && on_own_base && full_stock) ? 1.0f : 0.0f;

        if (has_prev) {
            float trees_delta = sc[WBGYM_S_TREES] - psc[WBGYM_S_TREES];
            comp[RC_TREES_FARMED] = trees_delta > 0 ? trees_delta : 0.0f;
        }

        /* ammo_conservation: pill captures * current shell fraction */
        float pill_cap_count = comp[RC_PILL_CAPTURED]; /* may be 0 if cat3 skipped */
        if (pill_cap_count == 0.0f && w[RC_PILL_CAPTURED] == 0.0f) {
            pill_cap_count = (float)gymCountEvents(obs, WBGYM_EVENT_PILL_CAPTURED);
        }
        comp[RC_AMMO_CONSERVATION] = pill_cap_count * sc[WBGYM_S_SHELLS];

        /* idle_penalty */
        bool idle = alive && (sc[WBGYM_S_SPEED] < 0.05f) && !on_own_base;
        if (action != NULL) {
            idle = idle && (action->shoot == 0) && (action->build_action == 0);
        }
        comp[RC_IDLE_PENALTY] = idle ? 1.0f : 0.0f;
    }

    /* ── CATEGORY 7: Mining ── */
    if (w[RC_MINE_PLACED] != 0.0f || w[RC_MINE_KILL] != 0.0f ||
        w[RC_MINE_PLACED_DEFENSIVE] != 0.0f || w[RC_MINE_PLACED_ON_ROAD] != 0.0f ||
        w[RC_OWN_MINE_HIT] != 0.0f) {

        bool laid_mine = (action != NULL && action->lay_mine > 0);
        comp[RC_MINE_PLACED] = laid_mine ? 1.0f : 0.0f;

        /* Track mine positions */
        if (laid_mine) {
            int midx = rs->mine_idx % WBGYM_MINE_BUFFER;
            rs->mine_positions[midx][0] = tank_x;
            rs->mine_positions[midx][1] = tank_y;
            rs->mine_valid[midx] = true;
            rs->mine_idx++;
        }

        /* mine_kill: check if any tracked mine is near an explosion sound */
        bool mine_kill = false;
        for (int si = 0; si < obs->num_sounds && !mine_kill; si++) {
            if (obs->sounds[si].type != WBGYM_SND_EXPLOSION) continue;
            float sx = tank_x + obs->sounds[si].rx;
            float sy = tank_y + obs->sounds[si].ry;
            for (int mi = 0; mi < WBGYM_MINE_BUFFER; mi++) {
                if (!rs->mine_valid[mi]) continue;
                float dx = rs->mine_positions[mi][0] - sx;
                float dy = rs->mine_positions[mi][1] - sy;
                if (dx*dx + dy*dy < 4.0f) {
                    mine_kill = true;
                    break;
                }
            }
        }
        comp[RC_MINE_KILL] = mine_kill ? 1.0f : 0.0f;

        /* mine_placed_defensive: mine just placed near own base */
        if (laid_mine) {
            bool near_base = false;
            int placed_idx = (rs->mine_idx - 1) % WBGYM_MINE_BUFFER;
            float mx = rs->mine_positions[placed_idx][0];
            float my = rs->mine_positions[placed_idx][1];
            for (int bi = 0; bi < obs->num_bases; bi++) {
                if (obs->bases[bi].owner != WBGYM_OWNER_SELF) continue;
                float dx = mx - (float)obs->bases[bi].tx;
                float dy = my - (float)obs->bases[bi].ty;
                if (dx*dx + dy*dy <= 25.0f) {
                    near_base = true;
                    break;
                }
            }
            comp[RC_MINE_PLACED_DEFENSIVE] = near_base ? 1.0f : 0.0f;
        }

        /* mine_placed_on_road: center terrain tile is road */
        if (laid_mine) {
            float center = obs->terrain[14][14];
            bool is_road = fabsf(center - (float)WBGYM_TERRAIN_ROAD / WBGYM_TERRAIN_NORM) < 0.01f;
            comp[RC_MINE_PLACED_ON_ROAD] = is_road ? 1.0f : 0.0f;
        }

        /* own_mine_hit: self near explosion AND received hit */
        bool has_hit_received = gymCountEvents(obs, WBGYM_EVENT_HIT_RECEIVED) > 0;
        if (has_hit_received) {
            bool self_near_exp = false;
            for (int si = 0; si < obs->num_sounds; si++) {
                if (obs->sounds[si].type != WBGYM_SND_EXPLOSION) continue;
                float dx = obs->sounds[si].rx;
                float dy = obs->sounds[si].ry;
                if (dx*dx + dy*dy < 4.0f) {
                    self_near_exp = true;
                    break;
                }
            }
            comp[RC_OWN_MINE_HIT] = self_near_exp ? 1.0f : 0.0f;
        }
    }

    /* ── Precompute ownership counts for categories 8 & 9 ── */
    int own_pill_count = 0, enemy_pill_count = 0;
    int own_base_count = 0, enemy_base_count = 0;
    float own_base_n = 0.0f, enemy_base_n = 0.0f;
    bool need_territory = (w[RC_OFFENSIVE_PRESSURE] != 0.0f || w[RC_DEFENSIVE_COVERAGE] != 0.0f ||
                          w[RC_ROAD_BUILT] != 0.0f || w[RC_WALL_BUILT] != 0.0f ||
                          w[RC_FLANK_BONUS] != 0.0f);
    bool need_strategic = (w[RC_INITIATIVE_SCORE] != 0.0f || w[RC_SPIKE_QUALITY] != 0.0f ||
                          w[RC_DONT_CARRY_TOO_MANY] != 0.0f);

    if (need_territory || need_strategic) {
        for (int i = 0; i < obs->num_pillboxes; i++) {
            if (obs->pillboxes[i].owner == WBGYM_OWNER_SELF) own_pill_count++;
            else if (obs->pillboxes[i].owner == WBGYM_OWNER_ENEMY) enemy_pill_count++;
        }
        for (int i = 0; i < obs->num_bases; i++) {
            if (obs->bases[i].owner == WBGYM_OWNER_SELF) { own_base_count++; own_base_n++; }
            else if (obs->bases[i].owner == WBGYM_OWNER_ENEMY) { enemy_base_count++; enemy_base_n++; }
        }
    }

    /* ── CATEGORY 8: Territory ── */
    if (need_territory) {
        /* offensive_pressure: own pills near enemy bases */
        if (alive && enemy_base_n > 0) {
            float pressure_sum = 0.0f;
            for (int bi = 0; bi < obs->num_bases; bi++) {
                if (obs->bases[bi].owner != WBGYM_OWNER_ENEMY) continue;
                float best_dist = 1e8f;
                for (int pi = 0; pi < obs->num_pillboxes; pi++) {
                    if (obs->pillboxes[pi].owner != WBGYM_OWNER_SELF) continue;
                    float dx = (float)obs->bases[bi].tx - (float)obs->pillboxes[pi].tx;
                    float dy = (float)obs->bases[bi].ty - (float)obs->pillboxes[pi].ty;
                    float d2 = dx*dx + dy*dy;
                    if (d2 < best_dist) best_dist = d2;
                }
                pressure_sum += 1.0f / (1.0f + sqrtf(best_dist));
            }
            comp[RC_OFFENSIVE_PRESSURE] = pressure_sum / enemy_base_n;
        }

        /* defensive_coverage: own bases with own pill nearby */
        if (alive && own_base_n > 0) {
            float covered = 0.0f;
            for (int bi = 0; bi < obs->num_bases; bi++) {
                if (obs->bases[bi].owner != WBGYM_OWNER_SELF) continue;
                for (int pi = 0; pi < obs->num_pillboxes; pi++) {
                    if (obs->pillboxes[pi].owner != WBGYM_OWNER_SELF) continue;
                    float dx = (float)obs->bases[bi].tx - (float)obs->pillboxes[pi].tx;
                    float dy = (float)obs->bases[bi].ty - (float)obs->pillboxes[pi].ty;
                    if (dx*dx + dy*dy <= 100.0f) {
                        covered += 1.0f;
                        break;
                    }
                }
            }
            comp[RC_DEFENSIVE_COVERAGE] = covered / own_base_n;
        }

        /* road_built / wall_built: successful build of road/wall */
        float successful_build = comp[RC_SUCCESSFUL_BUILD];
        comp[RC_ROAD_BUILT] = successful_build *
            (rs->active_build_action == WBGYM_BUILD_ROAD ? 1.0f : 0.0f);
        comp[RC_WALL_BUILT] = successful_build *
            (rs->active_build_action == WBGYM_BUILD_WALL ? 1.0f : 0.0f);

        /* flank_bonus: capture happened far from frontline */
        float pill_cap_count = comp[RC_PILL_CAPTURED];
        float base_cap_count = comp[RC_BASE_CAPTURED];
        if (pill_cap_count == 0.0f && w[RC_PILL_CAPTURED] == 0.0f)
            pill_cap_count = (float)gymCountEvents(obs, WBGYM_EVENT_PILL_CAPTURED);
        if (base_cap_count == 0.0f && w[RC_BASE_CAPTURED] == 0.0f)
            base_cap_count = (float)gymCountEvents(obs, WBGYM_EVENT_BASE_CAPTURED);

        if (pill_cap_count + base_cap_count > 0) {
            /* Compute centroids */
            float own_cx = 0, own_cy = 0, enemy_cx = 0, enemy_cy = 0;
            float own_n = own_pill_count > 0 ? (float)own_pill_count : 1.0f;
            float enemy_n = enemy_pill_count > 0 ? (float)enemy_pill_count : 1.0f;
            for (int i = 0; i < obs->num_pillboxes; i++) {
                if (obs->pillboxes[i].owner == WBGYM_OWNER_SELF) {
                    own_cx += (float)obs->pillboxes[i].tx;
                    own_cy += (float)obs->pillboxes[i].ty;
                } else if (obs->pillboxes[i].owner == WBGYM_OWNER_ENEMY) {
                    enemy_cx += (float)obs->pillboxes[i].tx;
                    enemy_cy += (float)obs->pillboxes[i].ty;
                }
            }
            own_cx /= own_n; own_cy /= own_n;
            enemy_cx /= enemy_n; enemy_cy /= enemy_n;
            float war_cx = (own_cx + enemy_cx) / 2.0f;
            float war_cy = (own_cy + enemy_cy) / 2.0f;
            float dx = tank_x - war_cx;
            float dy = tank_y - war_cy;
            float dist_to_war = sqrtf(dx*dx + dy*dy);
            comp[RC_FLANK_BONUS] = (dist_to_war > 20.0f) ? 1.0f : 0.0f;
        }
    }

    /* ── CATEGORY 9: Strategic ── */
    if (need_strategic) {
        /* initiative_score */
        float pill_cap_s = comp[RC_PILL_CAPTURED];
        float pill_lost_s = comp[RC_PILL_LOST];
        float base_cap_s = comp[RC_BASE_CAPTURED];
        float base_lost_s = comp[RC_BASE_LOST];
        /* Recount if categories were skipped */
        if (pill_cap_s == 0.0f && w[RC_PILL_CAPTURED] == 0.0f)
            pill_cap_s = (float)gymCountEvents(obs, WBGYM_EVENT_PILL_CAPTURED);
        if (pill_lost_s == 0.0f && w[RC_PILL_LOST] == 0.0f)
            pill_lost_s = (float)gymCountEvents(obs, WBGYM_EVENT_PILL_LOST);
        if (base_cap_s == 0.0f && w[RC_BASE_CAPTURED] == 0.0f)
            base_cap_s = (float)gymCountEvents(obs, WBGYM_EVENT_BASE_CAPTURED);
        if (base_lost_s == 0.0f && w[RC_BASE_LOST] == 0.0f)
            base_lost_s = (float)gymCountEvents(obs, WBGYM_EVENT_BASE_LOST);

        float tick_captures = pill_cap_s + base_cap_s;
        float tick_losses = pill_lost_s + base_lost_s;
        int iidx = rs->init_idx % WBGYM_INITIATIVE_WINDOW;
        rs->init_captures[iidx] = tick_captures;
        rs->init_losses[iidx] = tick_losses;
        rs->init_idx++;

        float init_sum = 0.0f;
        for (int i = 0; i < WBGYM_INITIATIVE_WINDOW; i++) {
            init_sum += rs->init_captures[i] - rs->init_losses[i];
        }
        comp[RC_INITIATIVE_SCORE] = alive ? init_sum : 0.0f;

        /* spike_quality: >= 3 own pills within 9 tiles of an enemy base */
        bool spike_detected = false;
        for (int bi = 0; bi < obs->num_bases && !spike_detected; bi++) {
            if (obs->bases[bi].owner != WBGYM_OWNER_ENEMY) continue;
            int pills_near = 0;
            for (int pi = 0; pi < obs->num_pillboxes; pi++) {
                if (obs->pillboxes[pi].owner != WBGYM_OWNER_SELF) continue;
                float dx = (float)obs->pillboxes[pi].tx - (float)obs->bases[bi].tx;
                float dy = (float)obs->pillboxes[pi].ty - (float)obs->bases[bi].ty;
                if (dx*dx + dy*dy <= 81.0f) pills_near++;
            }
            if (pills_near >= 3) spike_detected = true;
        }
        comp[RC_SPIKE_QUALITY] = spike_detected ? 1.0f : 0.0f;
        if (spike_detected) rs->last_spike_tick = rs->tick;

        /* dont_carry_too_many */
        float carried = sc[WBGYM_S_PILL_COUNT] * 16.0f;
        bool recent_spike = (rs->tick - rs->last_spike_tick) < 50;
        comp[RC_DONT_CARRY_TOO_MANY] =
            (alive && carried > 2.0f && !recent_spike) ? 1.0f : 0.0f;
    }

    /* ── CATEGORY 10: Multi-agent (placeholders — always zero) ── */
    /* comp[RC_DECOY_ASSIST] = 0; comp[RC_TEAM_COORDINATION] = 0; comp[RC_MESSAGE_USEFUL] = 0; */

    /* ── CATEGORY 11: Phase 1 Shaping ── */
    if (w[RC_EXPLORATION_BONUS] != 0.0f || w[RC_BASE_PROXIMITY] != 0.0f ||
        w[RC_SPEED_BONUS] != 0.0f || w[RC_BOAT_OVERSTAY] != 0.0f ||
        w[RC_ON_LAND_BONUS] != 0.0f || w[RC_APPROACH_PILLBOX] != 0.0f ||
        w[RC_FACING_PILLBOX] != 0.0f) {

        /* exploration_bonus: bitfield tracking */
        if (alive && w[RC_EXPLORATION_BONUS] != 0.0f) {
            int tx = (int)tank_x;
            int ty = (int)tank_y;
            if (tx >= 0 && tx < 256 && ty >= 0 && ty < 256) {
                int byte_idx = tx / 8;
                uint8_t bit = (uint8_t)(1 << (tx % 8));
                if (!(rs->visited_tiles[ty][byte_idx] & bit)) {
                    rs->visited_tiles[ty][byte_idx] |= bit;
                    rs->visited_count++;
                    float n_visited = (float)rs->visited_count;
                    float raw = (float)WBGYM_EXPLORATION_FULL / n_visited;
                    comp[RC_EXPLORATION_BONUS] = raw < 1.0f ? raw : 1.0f;
                }
            }
        }

        /* base_proximity: alive and near any base */
        if (alive && w[RC_BASE_PROXIMITY] != 0.0f) {
            bool near_base = false;
            for (int bi = 0; bi < obs->num_bases; bi++) {
                float dx = tank_x - (float)obs->bases[bi].tx;
                float dy = tank_y - (float)obs->bases[bi].ty;
                if (dx*dx + dy*dy <= 9.0f) {
                    near_base = true;
                    break;
                }
            }
            comp[RC_BASE_PROXIMITY] = near_base ? 1.0f : 0.0f;
        }

        /* speed_bonus */
        comp[RC_SPEED_BONUS] = alive ? sc[WBGYM_S_SPEED] : 0.0f;

        /* boat_overstay */
        bool on_boat = sc[WBGYM_S_IN_BOAT] > 0.5f;
        rs->boat_ticks = on_boat ? rs->boat_ticks + 1 : 0;
        comp[RC_BOAT_OVERSTAY] =
            (alive && on_boat && rs->boat_ticks > WBGYM_BOAT_GRACE_TICKS) ? 1.0f : 0.0f;

        /* on_land_bonus: 1.0 every tick the tank is alive and not on boat */
        comp[RC_ON_LAND_BONUS] = (alive && !on_boat) ? 1.0f : 0.0f;

        /* approach_pillbox: delta-based, reward getting closer to nearest non-owned pill */
        if (alive && w[RC_APPROACH_PILLBOX] != 0.0f) {
            float min_dist = 9999.0f;
            for (int pi = 0; pi < obs->num_pillboxes; pi++) {
                if (obs->pillboxes[pi].owner == WBGYM_OWNER_SELF ||
                    obs->pillboxes[pi].owner == WBGYM_OWNER_ALLY)
                    continue;
                float dx = tank_x - (float)obs->pillboxes[pi].tx;
                float dy = tank_y - (float)obs->pillboxes[pi].ty;
                float d = sqrtf(dx*dx + dy*dy);
                if (d < min_dist) min_dist = d;
            }
            if (min_dist < 9999.0f && has_prev && rs->prev_nearest_pill_dist > 0.0f) {
                comp[RC_APPROACH_PILLBOX] = rs->prev_nearest_pill_dist - min_dist;
            }
            rs->prev_nearest_pill_dist = min_dist;
        }

        /* facing_pillbox: reward for aiming toward nearest non-owned pill */
        if (alive && w[RC_FACING_PILLBOX] != 0.0f) {
            /* scalar[5] = sin(dir), scalar[6] = cos(dir), where dir 0=north clockwise */
            float fx = obs->scalar[5];   /* east component */
            float fy = -obs->scalar[6];  /* south component (cos=1 at north = -y in tile coords) */
            float best_dot = -2.0f;
            for (int pi = 0; pi < obs->num_pillboxes; pi++) {
                if (obs->pillboxes[pi].owner == WBGYM_OWNER_SELF ||
                    obs->pillboxes[pi].owner == WBGYM_OWNER_ALLY)
                    continue;
                float dx = (float)obs->pillboxes[pi].tx - tank_x;
                float dy = (float)obs->pillboxes[pi].ty - tank_y;
                float dist = sqrtf(dx*dx + dy*dy);
                if (dist < 0.5f) continue;
                float dot = (fx*dx + fy*dy) / dist;
                if (dot > best_dot) best_dot = dot;
            }
            if (best_dot > -2.0f) {
                comp[RC_FACING_PILLBOX] = fmaxf(0.0f, best_dot);
            }
        }
    }

    /* ── Combine weighted reward ── */
    float reward = 0.0f;
    for (int i = 0; i < WBGYM_NUM_REWARD_COMPONENTS; i++) {
        if (w[i] != 0.0f && comp[i] != 0.0f) {
            reward += w[i] * comp[i];
        }
    }
    obs->reward = reward;

    /* ── Update rolling state ── */
    memcpy(rs->prev_scalars, sc, sizeof(rs->prev_scalars));
    for (int i = 0; i < obs->num_pillboxes && i < WBGYM_MAX_PILLBOXES; i++) {
        rs->prev_pills[i][0] = (float)obs->pillboxes[i].tx;
        rs->prev_pills[i][1] = (float)obs->pillboxes[i].ty;
        rs->prev_pills[i][2] = (float)obs->pillboxes[i].owner;
        rs->prev_pills[i][3] = (float)obs->pillboxes[i].armor;
    }
    rs->prev_pill_count = obs->num_pillboxes;
    for (int i = 0; i < obs->num_bases && i < WBGYM_MAX_BASES; i++) {
        rs->prev_bases[i][0] = (float)obs->bases[i].tx;
        rs->prev_bases[i][1] = (float)obs->bases[i].ty;
        rs->prev_bases[i][2] = (float)obs->bases[i].owner;
        rs->prev_bases[i][3] = (float)obs->bases[i].shells;
        rs->prev_bases[i][4] = (float)obs->bases[i].mines;
        rs->prev_bases[i][5] = (float)obs->bases[i].armour;
    }
    rs->prev_base_count = obs->num_bases;
    rs->prev_lgm_status = lgm_status;
    rs->has_prev = true;
    rs->tick++;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

WBGYM_API WinBoloGym *winbolo_create(const char *map_path, int game_type) {
    WinBoloGym *g = (WinBoloGym *)calloc(1, sizeof(WinBoloGym));
    if (g == NULL) return NULL;

    g->controlSub = SUBSCRIBER_HANDLE_INVALID;
    g->gameMode = (gameType)game_type;
    guiMessageSetHandler(gymMessageHandler);
    wsim_init_tables();  /* Ensure trig tables are ready before any stepping */

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
    InputPacket pkt = {0};
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
        if (action->gun_range_adjust > 0)  pkt.flags |= (1 << INPUT_FLAG_GUNSIGHT_SHIFT);
        if (action->gun_range_adjust < 0)  pkt.flags |= (2 << INPUT_FLAG_GUNSIGHT_SHIFT);
    }

    /* Game tick — send input to server and tick */
    game->cachedEventCount = 0;
    game->transport.sendInput(game->transport.ctx, &pkt);
    game->transport.tick(game->transport.ctx);
    gymBufferServerEvents(game);
    game->simTickCounter++;
    game->gameTickCount++;

    /* Keys tick — replay held buttons */
    InputPacket keysPkt = {0};
    keysPkt.tick = game->simTickCounter;
    keysPkt.playerNum = 0;
    keysPkt.buttons = pkt.buttons;

    game->transport.recordInput(game->transport.ctx, &keysPkt);
    game->transport.tick(game->transport.ctx);
    gymBufferServerEvents(game);
    game->simTickCounter++;

    gymBuildObs(game, obs_out);

    if (game->rewardsEnabled) {
        gymComputeRewardsMut(game, obs_out, action);
    }
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
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < count; i++) {
        winbolo_step(games[i], &actions[i], &obs_out[i]);
    }
}

WBGYM_API void winbolo_fill_actions_batch(
    const int32_t *actions_flat,
    WinBoloAction *actions_out,
    int count
) {
    static const int accel_map[3] = {-1, 0, 1};
    static const int turn_map[3] = {-1, 0, 1};
    static const int gun_range_map[3] = {-1, 0, 1};

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < count; i++) {
        const int32_t *a = &actions_flat[i * 8];
        WinBoloAction *out = &actions_out[i];
        out->accel            = accel_map[a[0]];
        out->turn             = turn_map[a[1]];
        out->shoot            = a[2];
        out->lay_mine         = a[3];
        out->gun_range_adjust = gun_range_map[a[4]];
        out->build_action     = a[5];
        out->build_rx         = a[6] - 14;
        out->build_ry         = a[7] - 14;
    }
}

WBGYM_API void winbolo_obs_to_numpy_batch(
    const WinBoloObs *obs_array,
    float *terrain_out,
    float *scalar_out,
    float *entities_out,
    float *entity_mask_out,
    float *sounds_out,
    float *sound_mask_out,
    int count
) {
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < count; i++) {
        const WinBoloObs *obs = &obs_array[i];

        /* Terrain: [29, 29, 2] — channel-last interleaved */
        float *t = &terrain_out[i * WBGYM_SPATIAL_SIZE * WBGYM_SPATIAL_SIZE * 2];
        for (int r = 0; r < WBGYM_SPATIAL_SIZE; r++) {
            for (int c = 0; c < WBGYM_SPATIAL_SIZE; c++) {
                int idx = (r * WBGYM_SPATIAL_SIZE + c) * 2;
                t[idx + 0] = obs->terrain[r][c];
                t[idx + 1] = obs->mines_map[r][c];
            }
        }

        /* Scalars: direct copy */
        memcpy(&scalar_out[i * WBGYM_NUM_SCALARS],
               obs->scalar, WBGYM_NUM_SCALARS * sizeof(float));

        /* Entities: [256, 7] with normalization matching obs_to_tensors() */
        float *e_out  = &entities_out[i * WBGYM_MAX_ENTITIES * 7];
        float *em_out = &entity_mask_out[i * WBGYM_MAX_ENTITIES];
        int n_ent = obs->num_entities < WBGYM_MAX_ENTITIES
                    ? obs->num_entities : WBGYM_MAX_ENTITIES;

        for (int j = 0; j < n_ent; j++) {
            const WinBoloEntity *e = &obs->entities[j];
            float *row = &e_out[j * 7];
            row[0] = e->rx;
            row[1] = e->ry;
            row[2] = (float)e->type / 6.0f;
            row[3] = (float)e->allegiance / 3.0f;
            row[4] = e->direction;
            row[5] = e->speed;
            row[6] = e->strength;
            em_out[j] = 1.0f;
        }
        /* Zero remaining slots (0.0f == all-zero bits) */
        if (n_ent < WBGYM_MAX_ENTITIES) {
            memset(&e_out[n_ent * 7], 0,
                   (WBGYM_MAX_ENTITIES - n_ent) * 7 * sizeof(float));
            memset(&em_out[n_ent], 0,
                   (WBGYM_MAX_ENTITIES - n_ent) * sizeof(float));
        }

        /* Sounds: [32, 4] with normalization matching obs_to_tensors() */
        float *s_out  = &sounds_out[i * WBGYM_MAX_SOUNDS * 4];
        float *sm_out = &sound_mask_out[i * WBGYM_MAX_SOUNDS];
        int n_snd = obs->num_sounds < WBGYM_MAX_SOUNDS
                    ? obs->num_sounds : WBGYM_MAX_SOUNDS;

        for (int j = 0; j < n_snd; j++) {
            const WinBoloSoundEvent *s = &obs->sounds[j];
            float *row = &s_out[j * 4];
            row[0] = s->rx / 40.0f;
            row[1] = s->ry / 40.0f;
            row[2] = (float)s->type / 6.0f;
            row[3] = (float)s->allegiance / 3.0f;
            sm_out[j] = 1.0f;
        }
        if (n_snd < WBGYM_MAX_SOUNDS) {
            memset(&s_out[n_snd * 4], 0,
                   (WBGYM_MAX_SOUNDS - n_snd) * 4 * sizeof(float));
            memset(&sm_out[n_snd], 0,
                   (WBGYM_MAX_SOUNDS - n_snd) * sizeof(float));
        }
    }
}

WBGYM_API void winbolo_obs_to_reward_batch(
    const WinBoloObs *obs_array,
    WinBoloRewardBatchOut *out,
    int count
) {
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < count; i++) {
        const WinBoloObs *obs = &obs_array[i];

        /* Scalars: direct copy */
        memcpy(&out->scalars[i * WBGYM_NUM_SCALARS],
               obs->scalar, WBGYM_NUM_SCALARS * sizeof(float));

        /* Events */
        int n_ev = obs->num_events < WBGYM_MAX_EVENTS
                   ? obs->num_events : WBGYM_MAX_EVENTS;
        out->event_count[i] = n_ev;
        memcpy(&out->events[i * WBGYM_MAX_EVENTS], obs->events, n_ev);
        if (n_ev < WBGYM_MAX_EVENTS)
            memset(&out->events[i * WBGYM_MAX_EVENTS + n_ev], 0,
                   WBGYM_MAX_EVENTS - n_ev);

        /* Pillboxes: [16, 4] = (tx, ty, owner, armor) */
        int n_p = obs->num_pillboxes < WBGYM_MAX_PILLBOXES
                  ? obs->num_pillboxes : WBGYM_MAX_PILLBOXES;
        out->pill_count[i] = n_p;
        float *p_out = &out->pills[i * WBGYM_MAX_PILLBOXES * 4];
        for (int j = 0; j < n_p; j++) {
            const WinBoloPillObs *p = &obs->pillboxes[j];
            float *row = &p_out[j * 4];
            row[0] = (float)p->tx;
            row[1] = (float)p->ty;
            row[2] = (float)p->owner;
            row[3] = (float)p->armor;
        }
        if (n_p < WBGYM_MAX_PILLBOXES)
            memset(&p_out[n_p * 4], 0,
                   (WBGYM_MAX_PILLBOXES - n_p) * 4 * sizeof(float));

        /* Bases: [16, 6] = (tx, ty, owner, shells, mines, armour) */
        int n_b = obs->num_bases < WBGYM_MAX_BASES
                  ? obs->num_bases : WBGYM_MAX_BASES;
        out->base_count[i] = n_b;
        float *b_out = &out->bases[i * WBGYM_MAX_BASES * 6];
        for (int j = 0; j < n_b; j++) {
            const WinBoloBaseObs *b = &obs->bases[j];
            float *row = &b_out[j * 6];
            row[0] = (float)b->tx;
            row[1] = (float)b->ty;
            row[2] = (float)b->owner;
            row[3] = (float)b->shells;
            row[4] = (float)b->mines;
            row[5] = (float)b->armour;
        }
        if (n_b < WBGYM_MAX_BASES)
            memset(&b_out[n_b * 6], 0,
                   (WBGYM_MAX_BASES - n_b) * 6 * sizeof(float));

        /* Entities: [256, 9] = (rx, ry, type, allegiance, dir, speed, strength, flags, id) — raw, no normalization */
        int n_e = obs->num_entities < WBGYM_MAX_ENTITIES
                  ? obs->num_entities : WBGYM_MAX_ENTITIES;
        out->entity_count[i] = n_e;
        float *e_out = &out->entities[i * WBGYM_MAX_ENTITIES * 9];
        for (int j = 0; j < n_e; j++) {
            const WinBoloEntity *e = &obs->entities[j];
            float *row = &e_out[j * 9];
            row[0] = e->rx;
            row[1] = e->ry;
            row[2] = (float)e->type;
            row[3] = (float)e->allegiance;
            row[4] = e->direction;
            row[5] = e->speed;
            row[6] = e->strength;
            row[7] = (float)e->flags;
            row[8] = (float)e->id;
        }
        if (n_e < WBGYM_MAX_ENTITIES)
            memset(&e_out[n_e * 9], 0,
                   (WBGYM_MAX_ENTITIES - n_e) * 9 * sizeof(float));

        /* Sounds: [32, 4] = (rx, ry, type, allegiance) — raw, no normalization */
        int n_s = obs->num_sounds < WBGYM_MAX_SOUNDS
                  ? obs->num_sounds : WBGYM_MAX_SOUNDS;
        out->sound_count[i] = n_s;
        float *s_out = &out->sounds[i * WBGYM_MAX_SOUNDS * 4];
        for (int j = 0; j < n_s; j++) {
            const WinBoloSoundEvent *s = &obs->sounds[j];
            float *row = &s_out[j * 4];
            row[0] = s->rx;
            row[1] = s->ry;
            row[2] = (float)s->type;
            row[3] = (float)s->allegiance;
        }
        if (n_s < WBGYM_MAX_SOUNDS)
            memset(&s_out[n_s * 4], 0,
                   (WBGYM_MAX_SOUNDS - n_s) * 4 * sizeof(float));

        /* Terrain: [29, 29] — just the terrain layer, no mines */
        memcpy(&out->terrain[i * WBGYM_SPATIAL_SIZE * WBGYM_SPATIAL_SIZE],
               obs->terrain,
               WBGYM_SPATIAL_SIZE * WBGYM_SPATIAL_SIZE * sizeof(float));

        /* Scalar metadata */
        out->dead[i]          = obs->dead;
        out->game_over[i]     = obs->game_over;
        out->game_won[i]      = obs->game_won;
        out->tick[i]          = (int32_t)obs->tick;
        out->tank_x[i]        = obs->tank_x;
        out->tank_y[i]        = obs->tank_y;
        out->assistant_msg[i] = obs->assistant_msg;
    }
}

WBGYM_API void winbolo_set_reward_weights(WinBoloGym *game, const float *weights, int count) {
    if (game == NULL || weights == NULL) return;
    int n = count < WBGYM_NUM_REWARD_COMPONENTS ? count : WBGYM_NUM_REWARD_COMPONENTS;
    memcpy(game->rewardState.weights, weights, n * sizeof(float));
    game->rewardsEnabled = true;
}

WBGYM_API int winbolo_reward_component_count(void) {
    return WBGYM_NUM_REWARD_COMPONENTS;
}
